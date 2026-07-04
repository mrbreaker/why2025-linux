#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Heavy-load uptime test: heartbeat + sensorpanel for N minutes.

Boots the badge, logs in, detaches fbcon, launches sensorpanel in the
background, then runs the same fork-free heartbeat loop as
heartbeat_test.py. Sensorpanel reads BMI270 + BME680 every ~3 Hz and
draws to /dev/fb0, providing realistic continuous CPU + I²C + DMA
activity. Reproduces the silent kernel wedge described in
docs/KNOWN-ISSUES.md.

If the heartbeat tag stops appearing for >90 s we declare wedge and
attempt to capture state.

Usage:
    python3 tools/loadtest.py [max_min=30] [output_dir=/tmp/loadtest]
"""
import os, sys, re, time, serial, datetime

PORT = os.environ.get('BADGE_PORT', '/dev/cu.wchusbserial10')
BAUD = 115200
HEARTBEAT_INTERVAL_S = 30
HEARTBEAT_GAP_S = 90
HEARTBEAT_TAG = 'HBHBHB'

LOGIN_RE = re.compile(rb'(buildroot login:|why2025 login:)', re.IGNORECASE)
HB_RE = re.compile(rb'HBHBHB\s+(\d+\.\d+)', re.IGNORECASE)

BOOT_PROLOGUE = [
    (b'dmesg -n 1\n', 1.0),
    (b'echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null\n', 1.0),
    # sensorpanel writes to /dev/fb0 every frame (~3 Hz). The renderer
    # reads BMI270 + BME680 sysfs on each tick, so the I²C-gpio bus is
    # active throughout. Backgrounding it via the shell &.
    (b'/usr/bin/sensorpanel >/tmp/sp.log 2>&1 &\n', 3.0),
    # Sanity check sensorpanel via /proc lookup (busybox-minimal lacks pidof).
    (b'ls /proc/*/comm 2>/dev/null | head -50 | xargs grep -l sensorpanel 2>/dev/null; echo SP-CHECK-DONE\n', 3.0),
]

# Heartbeat emits HBHBHB tag + a snapshot of buddy/mem state every cycle.
# Fork-light implementation: read+echo+case are shell builtins, so the only
# fork per cycle is `sleep`. Earlier cat+grep version tripped a kernel
# wedge within ~1 minute under sensorpanel load (fork rate × workload =
# trigger, even though buddy/mem stayed flat — kernel deadlock, not OOM).
# Builtin-only approach keeps the badge alive long enough to characterise
# the wedge trajectory.
HEARTBEAT_SCRIPT = (
    '(while :; do '
    'read -r u < /proc/uptime; '
    f'echo "{HEARTBEAT_TAG} $u"; '
    'while read -r line; do echo "BUD $line"; done < /proc/buddyinfo; '
    'while read -r f rest; do '
    'case "$f" in MemTotal:|MemFree:|MemAvailable:|Slab:|SReclaimable:|SUnreclaim:) '
    'echo "MEM $f $rest";; esac; '
    'done < /proc/meminfo; '
    'echo "---"; '
    f'sleep {HEARTBEAT_INTERVAL_S}; '
    'done) &\n'
)


def main():
    max_min = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    base_outdir = sys.argv[2] if len(sys.argv) > 2 else '/tmp/loadtest'
    run_id = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    outdir = os.path.join(base_outdir, run_id)
    os.makedirs(outdir, exist_ok=True)
    sys.stderr.write(f'loadtest: writing transcript to {outdir}\n')
    sys.stderr.write(f'loadtest: max {max_min} min, gap {HEARTBEAT_GAP_S}s\n')
    sys.stderr.write(f'loadtest: workload = sensorpanel + heartbeat\n')

    s = serial.Serial(PORT, BAUD, timeout=0.3)
    transcript = bytearray()
    last_hb_uptime = None
    last_hb_walltime = None
    wedge_walltime = None
    classification = 'completed_clean'

    try:
        s.dtr = False; s.rts = False
        time.sleep(0.1)
        s.rts = True; time.sleep(0.2); s.rts = False

        deadline = time.time() + 60.0
        saw_login = False
        while time.time() < deadline:
            chunk = s.read(8192)
            if chunk:
                transcript.extend(chunk)
                if LOGIN_RE.search(transcript):
                    saw_login = True
                    break
        if not saw_login:
            classification = 'never_booted'
            raise RuntimeError('boot did not produce login banner in 60s')

        for ch in b'root\n':
            s.write(bytes([ch])); time.sleep(0.05)
        _drain(s, transcript, 2.0)

        for cmd, wait in BOOT_PROLOGUE:
            s.write(cmd)
            _drain(s, transcript, wait)

        s.write(HEARTBEAT_SCRIPT.encode('ascii'))
        _drain(s, transcript, 1.5)

        sys.stderr.write('loadtest: monitoring loop started\n')
        end_walltime = time.time() + max_min * 60.0
        last_status_print = 0.0

        # Rolling tail buffer so a heartbeat split across two pyserial
        # chunks still matches. 256 bytes covers any HB line + slack.
        tail = bytearray()
        while time.time() < end_walltime:
            chunk = s.read(8192)
            if chunk:
                transcript.extend(chunk)
                tail.extend(chunk)
                if len(tail) > 1024:
                    del tail[:len(tail) - 1024]
                last_match = None
                for m in HB_RE.finditer(tail):
                    last_match = m
                if last_match:
                    last_hb_uptime = float(last_match.group(1))
                    last_hb_walltime = time.time()
                    # Drop matched + earlier from tail so we don't re-match it.
                    del tail[:last_match.end()]
            now = time.time()
            if now - last_status_print > 60.0:
                last_status_print = now
                age = (now - last_hb_walltime) if last_hb_walltime else None
                age_str = f'{age:.0f}s ago' if age is not None else 'never'
                up_str = f'{last_hb_uptime:.1f}s' if last_hb_uptime else 'n/a'
                sys.stderr.write(f'  [{int(now-end_walltime+max_min*60)}s] last HB '
                                 f'{age_str}, uptime={up_str}\n')
                sys.stderr.flush()
            # Wedge detection: if we've SEEN at least one HB and the gap exceeds
            # threshold, flag wedge. Also detect "never started" (no HB at all
            # in first 60 s of monitoring) — that means the script never began.
            if last_hb_walltime and (now - last_hb_walltime) > HEARTBEAT_GAP_S:
                wedge_walltime = now
                classification = 'wedged'
                sys.stderr.write(f'  WEDGE DETECTED at {now-(end_walltime-max_min*60):.0f}s walltime, '
                                 f'last HB {now-last_hb_walltime:.0f}s ago\n')
                _capture_wedge_state(s, transcript)
                break
            elapsed = now - (end_walltime - max_min * 60)
            if not last_hb_walltime and elapsed > 60.0:
                wedge_walltime = now
                classification = 'no_heartbeat_ever'
                sys.stderr.write(f'  NO HEARTBEAT in first {elapsed:.0f}s — heartbeat script never ran or died early\n')
                _capture_wedge_state(s, transcript)
                break

    except RuntimeError as e:
        # never_booted etc. — already classified above.
        sys.stderr.write(f'  early-exit: {e}\n')
    except (OSError, serial.SerialException) as e:
        classification = f'serial_error: {e}'
        sys.stderr.write(f'  PORT-ERROR: {e}\n')

    finally:
        try: s.close()
        except Exception: pass

    transcript_path = os.path.join(outdir, 'transcript.log')
    with open(transcript_path, 'wb') as f: f.write(bytes(transcript))

    summary_path = os.path.join(outdir, 'summary.txt')
    with open(summary_path, 'w') as f:
        f.write(f'classification={classification}\n')
        f.write(f'max_minutes={max_min}\n')
        f.write(f'last_hb_uptime={last_hb_uptime}\n')
        f.write(f'last_hb_walltime={last_hb_walltime}\n')
        f.write(f'wedge_walltime={wedge_walltime}\n')
        f.write(f'transcript_bytes={len(transcript)}\n')

    sys.stderr.write(f'\n================ done ================\n')
    sys.stderr.write(f'  classification: {classification}\n')
    sys.stderr.write(f'  last HB uptime: {last_hb_uptime}\n')
    if wedge_walltime and last_hb_walltime:
        sys.stderr.write(f'  wedge gap: {wedge_walltime - last_hb_walltime:.0f}s\n')
    sys.stderr.write(f'  transcript: {transcript_path}\n')
    sys.stderr.write(f'  summary:    {summary_path}\n')


def _capture_wedge_state(s, transcript):
    sys.stderr.write('  capturing state\n')
    try:
        s.write(b'\n'); _drain(s, transcript, 1.0)
        s.write(b'echo ===WEDGE===\n'); _drain(s, transcript, 2.0)
        s.write(b'cat /proc/uptime 2>&1\n'); _drain(s, transcript, 2.0)
        s.write(b'cat /proc/buddyinfo 2>&1\n'); _drain(s, transcript, 2.0)
        s.write(b'head -10 /proc/meminfo 2>&1\n'); _drain(s, transcript, 2.0)
        s.write(b'pidof sensorpanel\n'); _drain(s, transcript, 2.0)
        s.write(b'tail -20 /tmp/sp.log 2>&1\n'); _drain(s, transcript, 2.0)
        s.write(b'dmesg | tail -50 2>&1\n'); _drain(s, transcript, 4.0)
        s.write(b'echo ===WEDGE-DONE===\n'); _drain(s, transcript, 2.0)
    except (OSError, serial.SerialException) as e:
        sys.stderr.write(f'  state-capture port error: {e}\n')


def _drain(s, transcript, secs):
    end = time.time() + secs
    while time.time() < end:
        c = s.read(8192)
        if c: transcript.extend(c)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Runtime-wedge test under grep load (no sensorpanel, no fb writes).

Replaces sensorpanel with a continuous grep loop over the squashfs root.
Same heartbeat-monitoring logic as loadtest.py. Was used to test the
hypothesis that the wedge needs sensorpanel-style fb traffic; result
was that grep alone is sufficient — the trigger is fork+exec churn,
not file-content-specific (see docs/KNOWN-ISSUES.md).

Workload:
  while :; do grep -rl DOESNOTEXIST / >/dev/null 2>&1; sleep 1; done &

That walks the rootfs tree (~78 files), does a small read of each
through squashfs/mtd-rom, and rate-limits the loop with sleep 1 so we're
not doing zero-pause grep forks. Each cycle: ~1 grep fork + 1 sleep
fork. Roughly the same fork rate as loadtest's heartbeat.

Usage:
    python3 tools/greptest.py [max_min=30] [outdir=/tmp/greptest]
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
    # Continuous grep over the rootfs. Output discarded so we don't
    # flood UART. -l so grep stops on first match per file (faster).
    (b'(while :; do grep -rl DOESNOTEXIST / >/dev/null 2>&1; sleep 1; done) &\n', 2.0),
    (b'echo GREP-STARTED\n', 1.0),
]

HEARTBEAT_SCRIPT = (
    '(while :; do '
    'read -r u < /proc/uptime; '
    f'echo "{HEARTBEAT_TAG} $u"; '
    f'sleep {HEARTBEAT_INTERVAL_S}; '
    'done) &\n'
)


def main():
    max_min = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    base_outdir = sys.argv[2] if len(sys.argv) > 2 else '/tmp/greptest'
    run_id = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    outdir = os.path.join(base_outdir, run_id)
    os.makedirs(outdir, exist_ok=True)
    sys.stderr.write(f'greptest: writing transcript to {outdir}\n')
    sys.stderr.write(f'greptest: max {max_min} min, gap {HEARTBEAT_GAP_S}s\n')
    sys.stderr.write(f'greptest: workload = grep loop (no sensorpanel, no fb writes)\n')

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

        sys.stderr.write('greptest: monitoring loop started\n')
        end_walltime = time.time() + max_min * 60.0
        last_status_print = 0.0

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
                sys.stderr.write(f'  NO HEARTBEAT in first {elapsed:.0f}s\n')
                _capture_wedge_state(s, transcript)
                break

    except RuntimeError as e:
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

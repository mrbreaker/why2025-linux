#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-sleep-immune wedge reproducer / fix-verifier.

Classification keys on the BADGE-emitted watchdog reset banner
(`HP_SYS_HP_WDT_RESET`), never on host wall-clock, so a laptop nap
cannot fabricate a false wedge. The badge prints that banner from ROM
only when the kernel-fed MWDT (patch 0019) is starved — i.e. a real
scheduler-death wedge.

Workload default is the pure shell-builtin busy loop
`(while :; do :; done) &` — guaranteed zero fork/exec/mm/signal churn.
`--isolated` skips even the login-noise heartbeat and drives the loop
with nothing else running (tests whether a *second* concurrent
interrupt source is needed to trigger the wedge).

Usage:
  wedge_verify.py --port /dev/cu.wchusbserial110 [--max-min 4] [--isolated]
  wedge_verify.py --port ... --workload '(while :; do /bin/true; done) &'
"""
import argparse, os, re, sys, time, datetime, serial

LOGIN_RE = re.compile(rb'(buildroot login:|why2025 login:)', re.IGNORECASE)
WDT_RE = re.compile(rb'HP_SYS_HP_WDT_RESET')
SAVEDPC_RE = re.compile(rb'Core0 Saved PC:(0x[0-9a-fA-F]+)')
BOOT_TIMEOUT_S = 80
DEFAULT_WORKLOAD = '(while :; do :; done) &'


def drain(s, transcript, secs):
    end = time.time() + secs
    while time.time() < end:
        c = s.read(8192)
        if c:
            transcript.extend(c)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port',
                    default=os.environ.get('BADGE_PORT', '/dev/cu.wchusbserial10'),
                    help='serial port (default: $BADGE_PORT or '
                         '/dev/cu.wchusbserial10)')
    ap.add_argument('--workload', default=DEFAULT_WORKLOAD)
    ap.add_argument('--max-min', type=float, default=4.0)
    ap.add_argument('--isolated', action='store_true',
                    help='no post-login activity besides the workload')
    ap.add_argument('--label', default='verify')
    args = ap.parse_args()

    run_id = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    outdir = f'/tmp/wedge_verify/{args.label}-{run_id}'
    os.makedirs(outdir, exist_ok=True)
    log = sys.stderr
    log.write(f'== wedge_verify label={args.label} max={args.max_min}min '
              f'isolated={args.isolated} ==\n')
    log.write(f'   workload: {args.workload}\n   outdir: {outdir}\n'); log.flush()

    s = serial.Serial(args.port, 115200, timeout=0.3)
    transcript = bytearray()
    classification = 'incomplete'
    saved_pc = None

    try:
        # RTS hard reset, flush stale bytes, sync on THIS boot.
        s.dtr = False; s.rts = False; time.sleep(0.1)
        s.rts = True; time.sleep(0.3); s.rts = False
        time.sleep(2.0)
        try: s.reset_input_buffer()
        except Exception: pass

        deadline = time.time() + BOOT_TIMEOUT_S
        saw_login = False
        while time.time() < deadline:
            c = s.read(8192)
            if c:
                transcript.extend(c)
                if LOGIN_RE.search(transcript):
                    saw_login = True; break
        if not saw_login:
            classification = 'never_booted'
            raise RuntimeError('no login banner')
        log.write('   [boot] login banner seen\n'); log.flush()

        # Robust login: sync on a per-attempt marker echoed by a live shell.
        shell_ready = False
        for attempt in range(6):
            marker = b'RDY%d_42' % attempt
            s.write(b'\n'); time.sleep(0.2)
            for ch in b'root\n':
                s.write(bytes([ch])); time.sleep(0.05)
            drain(s, transcript, 1.5)
            s.write(b'echo ' + marker + b'\n')
            end = time.time() + 5.0
            while time.time() < end:
                c = s.read(8192)
                if c:
                    transcript.extend(c)
                    if transcript.count(marker) >= 2:
                        shell_ready = True; break
            if shell_ready:
                break
        if not shell_ready:
            classification = 'login_failed'
            raise RuntimeError('no interactive shell')
        log.write('   [login] shell confirmed\n'); log.flush()

        if not args.isolated:
            s.write(b'dmesg -n 1\n'); drain(s, transcript, 1.0)

        # Launch workload and mark it.
        s.write((args.workload + '\n').encode()); drain(s, transcript, 0.5)
        s.write(b'echo LOAD-STARTED\n'); drain(s, transcript, 1.0)
        load_mark = len(transcript)
        log.write('   [load] workload launched; watching for WDT banner\n'); log.flush()

        # Watch the badge stream for the WDT reset banner (host-sleep-immune:
        # keyed on badge-emitted bytes, not host clock). Poll for readiness at
        # the end if no banner appears.
        end_wall = time.time() + args.max_min * 60.0
        last_print = 0.0
        while time.time() < end_wall:
            c = s.read(8192)
            if c:
                transcript.extend(c)
                seg = transcript[load_mark:]
                if WDT_RE.search(seg):
                    classification = 'WEDGED_wdt'
                    m = SAVEDPC_RE.search(seg)
                    saved_pc = m.group(1).decode() if m else None
                    log.write(f'   *** WDT RESET banner => WEDGE (Saved PC={saved_pc}) ***\n')
                    log.flush()
                    drain(s, transcript, 3.0)
                    break
            now = time.time()
            if now - last_print > 30.0:
                last_print = now
                log.write(f'   [{now-(end_wall-args.max_min*60):.0f}s] no WDT banner yet\n')
                log.flush()
        else:
            # No WDT banner. Confirm the badge is actually still alive.
            s.write(b'\necho ALIVE_$((21*2))\n')
            end = time.time() + 6.0
            alive = False
            while time.time() < end:
                c = s.read(8192)
                if c:
                    transcript.extend(c)
                    if b'ALIVE_42' in transcript[load_mark:]:
                        alive = True; break
            classification = 'NO_WEDGE_alive' if alive else 'NO_WEDGE_unresponsive'
            log.write(f'   *** no WDT banner in {args.max_min}min; shell '
                      f'{"responsive" if alive else "UNRESPONSIVE"} ***\n'); log.flush()

    except RuntimeError as e:
        log.write(f'   early-exit: {e}\n')
    except (OSError, serial.SerialException) as e:
        classification = f'serial_error: {e}'
        log.write(f'   PORT-ERROR: {e}\n')
    finally:
        try: s.close()
        except Exception: pass

    with open(os.path.join(outdir, 'transcript.log'), 'wb') as f:
        f.write(bytes(transcript))
    with open(os.path.join(outdir, 'summary.txt'), 'w') as f:
        f.write(f'label={args.label}\nworkload={args.workload}\n')
        f.write(f'isolated={args.isolated}\nclassification={classification}\n')
        f.write(f'saved_pc={saved_pc}\ntranscript_bytes={len(transcript)}\n')

    log.write(f'\n==== RESULT: {classification} (saved_pc={saved_pc}) ====\n')
    log.write(f'  transcript: {outdir}/transcript.log\n'); log.flush()
    terminal = ('WEDGED_wdt', 'NO_WEDGE_alive', 'NO_WEDGE_unresponsive')
    sys.exit(0 if classification in terminal else 2)


if __name__ == '__main__':
    main()

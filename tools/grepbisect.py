#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bisect which subtree, when grepped, triggers the runtime kernel wedge.

For each target directory, boots fresh, logs in, runs `grep -rl
DOESNOTEXIST <dir>` in foreground for up to 60 s, watches for kernel
silence. If silence after grep starts, classify as wedge. If grep
completes naturally and shell prompt returns, classify as survived.

Usage:
    python3 tools/grepbisect.py [outdir=/tmp/grepbisect]
"""
import os, sys, re, time, serial, datetime

PORT = os.environ.get('BADGE_PORT', '/dev/cu.wchusbserial10')
BAUD = 115200

LOGIN_RE = re.compile(rb'(buildroot login:|why2025 login:)', re.IGNORECASE)
DONE_RE = re.compile(rb'GREP-DONE-\d+', re.IGNORECASE)

# Targets to bisect. Each (label, command) — command runs grep against
# the subtree and prints GREP-DONE-<label> when finished.
TARGETS = [
    # Continuous loops — does sustained grep on a single subtree trigger?
    ('loop-etc',         b'while :; do grep -rl DOESNOTEXIST /etc 2>/dev/null; done\n'),
    ('loop-bin',         b'while :; do grep -rl DOESNOTEXIST /bin 2>/dev/null; done\n'),
    ('loop-sys-bus',     b'while :; do grep -rl DOESNOTEXIST /sys/bus 2>/dev/null; done\n'),
    ('loop-sys-class',   b'while :; do grep -rl DOESNOTEXIST /sys/class 2>/dev/null; done\n'),
    ('loop-sys-devices', b'while :; do grep -rl DOESNOTEXIST /sys/devices 2>/dev/null; done\n'),
    ('loop-proc',        b'while :; do grep -rl DOESNOTEXIST /proc 2>/dev/null; done\n'),
]


def main():
    base_outdir = sys.argv[1] if len(sys.argv) > 1 else '/tmp/grepbisect'
    run_id = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    outdir = os.path.join(base_outdir, run_id)
    os.makedirs(outdir, exist_ok=True)
    sys.stderr.write(f'grepbisect: writing transcripts to {outdir}\n')

    results = []

    for label, cmd in TARGETS:
        sys.stderr.write(f'\n=== target: {label} ===\n')
        try:
            classification, last_kernel_line, transcript = run_one(label, cmd, 60.0)
        except (OSError, serial.SerialException) as e:
            classification = f'serial_error: {e}'
            last_kernel_line = ''
            transcript = b''
            sys.stderr.write(f'  PORT-ERROR: {e}\n')
            time.sleep(2.0)

        path = os.path.join(outdir, f'grep-{label}-{classification}.log')
        with open(path, 'wb') as f:
            f.write(transcript)

        sys.stderr.write(f'  {classification}  -> {path} ({len(transcript)} bytes)\n')
        sys.stderr.write(f'  last kernel line: {last_kernel_line}\n')
        results.append((label, classification, last_kernel_line))
        time.sleep(2.0)

    sys.stderr.write('\n================ summary ================\n')
    for label, cls, last in results:
        sys.stderr.write(f'  {label:<6} {cls:<14} last="{last}"\n')

    summary_path = os.path.join(outdir, 'summary.txt')
    with open(summary_path, 'w') as f:
        for label, cls, last in results:
            f.write(f'{label}\t{cls}\t{last}\n')


HB_RE = re.compile(rb'TICK\s+(\d+\.\d+)', re.IGNORECASE)


def run_one(label, cmd, timeout_s):
    s = serial.Serial(PORT, BAUD, timeout=0.3)
    try:
        s.dtr = False; s.rts = False
        time.sleep(0.1)
        s.rts = True; time.sleep(0.2); s.rts = False

        out = bytearray()
        deadline = time.time() + 60.0
        saw_login = False
        while time.time() < deadline:
            chunk = s.read(8192)
            if chunk:
                out.extend(chunk)
                if LOGIN_RE.search(out):
                    saw_login = True
                    break
        if not saw_login:
            return 'never_booted', _last_printk_line(out), bytes(out)

        for ch in b'root\n':
            s.write(bytes([ch])); time.sleep(0.05)
        _drain(s, out, 1.5)
        s.write(b'dmesg -n 1\n'); _drain(s, out, 1.0)

        # Start a 5-second-cadence TICK heartbeat so we can detect the
        # wedge while grep runs. Builtin-only, 1 fork per cycle (sleep).
        hb = (b'(while :; do read -r u < /proc/uptime; '
              b'echo "TICK $u"; sleep 5; done) &\n')
        s.write(hb)
        _drain(s, out, 2.0)

        # Confirm at least one TICK before launching grep.
        end_warmup = time.time() + 8.0
        baseline_ticks = 0
        while time.time() < end_warmup:
            c = s.read(8192)
            if c:
                out.extend(c)
                baseline_ticks += len(HB_RE.findall(c))
                if baseline_ticks >= 1:
                    break
        if baseline_ticks == 0:
            return 'no_tick_pre', _last_printk_line(out), bytes(out)

        # Launch grep in background so the shell stays alive to echo TICKs.
        grep_bg = b'(' + cmd.rstrip(b'\n') + b') >/dev/null 2>&1 &\n'
        s.write(grep_bg)
        _drain(s, out, 1.0)

        # Watch ticks. If 30 s passes without one, wedge.
        end = time.time() + timeout_s
        last_tick = time.time()
        wedged = False
        while time.time() < end:
            chunk = s.read(8192)
            if chunk:
                out.extend(chunk)
                if HB_RE.search(chunk):
                    last_tick = time.time()
            if (time.time() - last_tick) > 30.0:
                wedged = True
                break

        return ('wedged' if wedged else 'survived',
                _last_printk_line(out), bytes(out))
    finally:
        try: s.close()
        except Exception: pass


def _last_printk_line(buf):
    txt = buf[-2048:].decode('ascii', errors='replace')
    lines = [l.strip() for l in txt.splitlines() if l.strip()]
    return lines[-1][:140] if lines else '(empty)'


def _drain(s, out, secs):
    end = time.time() + secs
    while time.time() < end:
        c = s.read(8192)
        if c: out.extend(c)


if __name__ == '__main__':
    main()

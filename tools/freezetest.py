#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cold-boot reliability harness for the WHY2025 badge.

Usage:
    python3 tools/freezetest.py [N=20] [boot_window_s=30] [outdir=/tmp/freezetest]

For each cycle:
1. RTS-pulse reset on /dev/cu.wchusbserial10
2. Capture UART for `boot_window_s` seconds while looking for the
   buildroot login banner (`why2025 login:` or generic `login:`).
3. If the banner arrives: classify SUCCESS, log in as root, silence
   printk (`dmesg -n 1`), and dump /tmp/boot.log + /tmp/boot.snap.* +
   the tail of dmesg back over UART so we can compare ANY two boots
   side-by-side. Save the captured stream to
   <outdir>/<run-id>/cycle-NN-success.log
4. If the banner doesn't arrive: classify FREEZE, save the captured
   stream to <outdir>/<run-id>/cycle-NN-freeze.log, and extract the
   LAST kernel printk line so the summary table tells us where each
   cycle wedged.

Prints a running success/freeze tally + a final summary.

Pre-flight: tio must be closed (it exclusive-locks the port). On port
errors (CH340 dropouts — gotcha #7) the cycle is logged and we move on.
"""
import os, re, sys, time, serial, datetime

PORT = os.environ.get('BADGE_PORT', '/dev/cu.wchusbserial10')
BAUD = 115200

LOGIN_RE = re.compile(rb'(why2025 login:|buildroot login:|\nlogin:)', re.IGNORECASE)
LAST_PRINTK_RE = re.compile(rb'^\[\s*\d+\.\d+\][^\n]*$', re.MULTILINE)


def main():
    n_cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    boot_window = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    base_outdir = sys.argv[3] if len(sys.argv) > 3 else '/tmp/freezetest'

    run_id = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    outdir = os.path.join(base_outdir, run_id)
    os.makedirs(outdir, exist_ok=True)
    sys.stderr.write(f'freezetest: writing transcripts to {outdir}\n')
    sys.stderr.write(f'freezetest: {n_cycles} cycles, {boot_window:.0f}s boot window\n')

    successes = 0
    freezes = 0
    errors = 0
    last_printks = []  # list of (cycle, classification, last_printk_or_summary)

    for cycle in range(1, n_cycles + 1):
        sys.stderr.write(f'\n=== cycle {cycle:02d}/{n_cycles} ===\n')
        sys.stderr.flush()
        try:
            classification, last_line, transcript = run_cycle(boot_window)
        except (OSError, serial.SerialException) as e:
            errors += 1
            sys.stderr.write(f'  PORT-ERROR: {e}\n')
            last_printks.append((cycle, 'error', str(e)))
            time.sleep(2.0)  # let the CH340 settle
            continue

        if classification == 'success':
            suffix = 'success'
        elif classification == 'freeze_alive':
            suffix = 'freeze_alive'
        else:
            suffix = 'freeze'
        path = os.path.join(outdir, f'cycle-{cycle:02d}-{suffix}.log')
        with open(path, 'wb') as f:
            f.write(transcript)

        if classification == 'success':
            successes += 1
            sys.stderr.write(f'  SUCCESS       -> {path} ({len(transcript)} bytes)\n')
        elif classification == 'freeze_alive':
            freezes += 1
            sys.stderr.write(f'  FREEZE-ALIVE  -> {path} ({len(transcript)} bytes) last="{last_line}"\n')
        else:
            freezes += 1
            sys.stderr.write(f'  FREEZE        -> {path} ({len(transcript)} bytes) last="{last_line}"\n')
        last_printks.append((cycle, classification, last_line))

        # Settle before the next RTS reset so trials stay independent. A
        # freeze leaves the badge wedged; it only recovers via the ~30 s
        # MWDT reset (patch 0019). Resetting again before that completes
        # hits the badge mid-reboot and truncates the next boot, producing
        # a false cascade of consecutive freezes. Wait it out after a
        # freeze; a short breather is enough after a clean boot (the badge
        # is already idle at a login prompt).
        time.sleep(45.0 if classification != 'success' else 2.0)

    # Summary.
    sys.stderr.write('\n')
    sys.stderr.write(f'================= summary =================\n')
    sys.stderr.write(f'  cycles:    {n_cycles}\n')
    sys.stderr.write(f'  successes: {successes}\n')
    sys.stderr.write(f'  freezes:   {freezes}\n')
    sys.stderr.write(f'  errors:    {errors}\n')
    if n_cycles > 0:
        rate = 100.0 * successes / max(1, n_cycles - errors)
        sys.stderr.write(f'  success%:  {rate:.1f}\n')
    sys.stderr.write('\nlast lines per cycle (chronological):\n')
    for cyc, cls, line in last_printks:
        sys.stderr.write(f'  {cyc:02d}  {cls:<7}  {line}\n')

    summary_path = os.path.join(outdir, 'summary.txt')
    with open(summary_path, 'w') as f:
        f.write(f'cycles={n_cycles}\nsuccesses={successes}\nfreezes={freezes}\nerrors={errors}\n')
        for cyc, cls, line in last_printks:
            f.write(f'{cyc:02d}\t{cls}\t{line}\n')
    sys.stderr.write(f'\nsummary: {summary_path}\n')


def run_cycle(boot_window: float):
    """Reset, capture, classify, dump artifacts on success.

    Returns (classification, last_line_text, full_transcript_bytes).
    classification is 'success' or 'freeze'.
    """
    s = serial.Serial(PORT, BAUD, timeout=0.3)
    try:
        # Manual hard reset (gotcha #2: tio must already be closed).
        s.dtr = False
        s.rts = False
        time.sleep(0.1)
        s.rts = True
        time.sleep(0.2)
        s.rts = False

        out = bytearray()
        deadline = time.time() + boot_window
        saw_login = False
        while time.time() < deadline:
            chunk = s.read(8192)
            if chunk:
                out.extend(chunk)
                if not saw_login and LOGIN_RE.search(out):
                    saw_login = True
                    break  # exit early to spend remaining time on dumps

        if not saw_login:
            # Banner never showed up. Try a login anyway: if userspace is
            # alive (just printk muted somehow), root\n will get echoed
            # back and we'll see a # prompt. Distinguishes "true wedge"
            # from "printk-path broken but kernel still scheduling".
            len_before = len(out)
            for ch in b'root\n':
                s.write(bytes([ch]))
                time.sleep(0.05)
            _drain(s, out, 3.0)
            new_bytes = bytes(out[len_before:])
            # Heuristic: if we see ANY new bytes that look like a shell
            # echo (root or #) it means userspace responded.
            alive = bool(new_bytes) and (b'root' in new_bytes or b'#' in new_bytes
                                         or b'\n' in new_bytes)
            classification = 'freeze_alive' if alive else 'freeze'
            last = _last_printk_line(out)
            return classification, last, bytes(out)

        # Success path: log in, silence printk, dump diagnostic files.
        # Slow-typed login because login(1) drops chars when fed as a single write.
        for ch in b'root\n':
            s.write(bytes([ch]))
            time.sleep(0.05)
        _drain(s, out, 2.0)

        s.write(b'dmesg -n 1\n'); _drain(s, out, 1.0)
        # Sentinel boundaries make grepping dumps trivial.
        s.write(b'echo "===BOOT-LOG-START==="\n'); _drain(s, out, 0.5)
        s.write(b'cat /tmp/boot.log 2>/dev/null; echo "===BOOT-LOG-END==="\n')
        _drain(s, out, 2.5)

        s.write(b'echo "===SNAP-START==="\n'); _drain(s, out, 0.3)
        # Snaps are written every 5 s by S99zfinal; up to /tmp/boot.snap.11.
        s.write(b'for i in /tmp/boot.snap.*; do echo "--- $i ---"; cat "$i"; done; echo "===SNAP-END==="\n')
        _drain(s, out, 4.0)

        s.write(b'echo "===DMESG-TAIL-START==="\n'); _drain(s, out, 0.3)
        s.write(b'dmesg | tail -120; echo "===DMESG-TAIL-END==="\n')
        _drain(s, out, 4.0)

        s.write(b'echo "===CYCLE-DONE==="\n'); _drain(s, out, 1.0)

        last = _last_printk_line(out)
        return 'success', last, bytes(out)
    finally:
        s.close()


def _drain(s, out, secs):
    end = time.time() + secs
    while time.time() < end:
        c = s.read(8192)
        if c:
            out.extend(c)


def _last_printk_line(buf: bytes) -> str:
    """Best-effort extraction of the last [   N.NN] printk line."""
    matches = LAST_PRINTK_RE.findall(buf)
    if matches:
        try:
            return matches[-1].decode('ascii', errors='replace').strip()
        except Exception:
            return repr(matches[-1])[:120]
    # Fall back to last non-blank textual line.
    txt = buf.decode('ascii', errors='replace')
    lines = [l.strip() for l in txt.splitlines() if l.strip()]
    return lines[-1][:120] if lines else '(no output)'


if __name__ == '__main__':
    main()

# Known issues

## Silent kernel wedge under sustained fork+exec churn

Reproducer (after login, badge boots cleanly):

```sh
(while :; do /bin/true; done) &
```

Kernel goes silent within seconds — no Oops, no `DETECT_HUNG_TASK`
output, no panic. UART input is also dead, so `tools/loadtest.py`'s
post-wedge state-capture commands return nothing. Recovery requires a
deep esptool reset. (With patches 0019+0020 — unverified on hardware —
a wedge should instead auto-reset via the MWDT within 30 s and leave
the pre-wedge console tail in pstore/ramoops; see RUNTIME-WEDGE.md.)

### What's been ruled out
- **Memory exhaustion.** `/proc/buddyinfo` and `/proc/meminfo` stay
  flat across all observed pre-wedge heartbeat samples. Order-7 (512 KB)
  and order-8 (1 MB) blocks are still free at the wedge moment.
- **Sensorpanel-specific.** Originally observed under sensorpanel +
  heartbeat; later confirmed reproducible with grep loops, then with
  pure `/bin/true` loops. Sensorpanel is sufficient but not necessary.
- **File-content-specific.** Continuous grep loops on `/etc`, `/bin`,
  `/sys`, `/proc`, etc. all eventually wedge. Single-shot greps that
  finish (no loop) survive.
- **UART output volume.** Tight CPU loops with no UART output also
  trigger the wedge.

### What it scales with
**Fork+exec rate × wall-time.** The kernel-side heartbeat (KWB kthread,
disabled in default builds, source at
`drivers/misc/esp32p4-watchdog-blink.c`) and the userspace heartbeat
both stop at the same wall-time → total kernel scheduler death, not
just userspace or just printk.

### Hypothesis
Bug in the NOMMU FLAT exec contiguous-block alloc/free path under
sustained churn. Each fork+exec of busybox needs an order-7 (512 KB)
contiguous block; under sustained allocate/free, something in
`mm/nommu.c` or `fs/binfmt_flat.c` reaches an unrecoverable state on
this RV32 + 32 MB PSRAM + ESP32-P4 v1.0 silicon. CPU stalls in a path
that doesn't service the timer IRQ.

### Diagnostic ceiling reached
No Oops because the trap-handler / printk path also dies. Cannot
diagnose further without JTAG / SWD or a non-printk dead-mans-switch.
See `docs/RUNTIME-WEDGE.md` for the planned next steps.

### Candidate fixes shipped (2026-07, NOT yet built or hardware-verified)

A code audit found three independent mechanisms that all match the
wedge's phenomenology (scales with fork+exec rate, silent, timer IRQ
stops). Each now has a hardening patch; none has been proven to be THE
cause — the `/bin/true` reproducer decides:

- **Patch 0014 — signal-path mcause poisoning.** Upstream
  `arch_do_signal_or_restart()` writes `regs->cause = -1UL` for every
  pending signal taken during a syscall. Because entry.S substitutes
  `EXC_SYSCALL` (8) into `PT_CAUSE`, that site is live (only
  `rt_sigreturn` had been fixed), and `-1UL` survives the exit shim as
  `mcause = INT=1 + reserved exccode` on `mret` — the same encoding
  family behind two previous invisible-trap-loop wedges. The
  default-ignored SIGCHLD queued at every child exit hits this path
  once per fork+exec iteration, matching the reproducer's rate scaling
  exactly.
- **Patch 0015 — exec-time cache-flush hazards.** ROM cache thunks ran
  with no IRQ exclusion (the ROM sync engine is non-reentrant; a
  DMA-sync from IRQ context could clobber an in-flight op), and every
  exec did a whole-L2 invalidate whose writeback→invalidate window
  destroyed any concurrently dirtied line — silent kernel-memory
  corruption proportional to exec rate.
- **Patch 0016 — one lost oneshot alarm kills the tick forever.**
  `set_next_event` never checked target-in-past; with HZ_PERIODIC the
  tick only re-arms from inside the tick ISR, and every configured
  watchdog (SOFTLOCKUP, HUNG_TASK, WQ_WATCHDOG) is tick-fed — which is
  precisely why the wedge produces no detector output.

Verification: rebuild, then `(while :; do /bin/true; done) &` (plus
`tools/greptest.py` for the slower classes). If the wedge persists,
rerun with each patch individually reverted to isolate. Also worth
adding to the matrix: a pure-syscall no-exec loop (busybox shell
`while :; do :; done`) — if that still wedges, all exec/mm paths are
exonerated in one experiment.

### Workaround for shipping
- Don't run sustained fork+exec loops. Real-user workloads (occasional
  shell commands, sensorpanel idle, wifi-connect) are safe.
- Demos that need continuous child-process churn should be ported to a
  single C process (one fork at startup, then long-running).

## Boot residual ~1/30 freeze at pwm-c6 line (deferred)

After all the pwm-c6 first-apply fixes (patch 0011), ~1 in 30 cold boots still wedges
silently right after the kernel prints
`esp-hosted-c6-pwm soc:pwm-c6: esp32-c6 backlight pwm registered`.
Suspected to be a deferred-probe-context race when pwm-backlight's
post-apply class-register path runs concurrently with another deferred
consumer. Low priority — the boot harness retries any way.

**Candidate contributing cause (unconfirmed, fix applied, pending hardware
verification):** patch 0011 only skips the *first* `.apply()` call;
every subsequent brightness change (including the one after the SECOND
`.apply()`, right after registration) goes through `cmd_set_backlight()`
in patch 0008's SPI command channel. That channel's timeout-cleanup path
had a bug — a caller whose own 500 ms wait timed out would unconditionally
null out `adapter->cur_cmd`/`cmd_resp` even when a *different* command was
still legitimately in flight, discarding that command's real response and
letting a second command start before the first one's response returned.
Patch 0008 now guards the clear with an ownership check (`cur_cmd_seq`,
see `esp_cmd_release_if_owner()`). This may reduce the residual freeze
rate, but does not fix the separate, still-open architectural gap
described next — a command can still be silently starved rather than
clobbering another one. Needs a `tools/freezetest.py` run to confirm any
change in frequency; not verified on hardware yet.

**Second candidate fix shipped (2026-07, pending hardware verification):**
patch 0018 adds value-level dedupe to pwm-esp-hosted-c6. The second
probe-time apply (pwm-backlight's post-register
`backlight_update_status`) computes exactly the C6 slave's boot duty
(`BL_DISPLAY_INITIAL_DUTY` = 76 = DT `default-brightness-level`), so
seeding a last-sent cache with 76 and eliding same-value writes removes
*all* backlight SPI traffic from the deferred-probe race window — the
only command this driver ever issued during the freeze window was that
redundant one. Verify together with the 0014-0017 batch via
`tools/freezetest.py` (~90 cycles for real confidence at a 1/30 rate).

**Related, still-open follow-up — `esp_cmd_work()` busy-bail doesn't
drain the queue:** when `adapter->cur_cmd` is already busy,
`esp_cmd_work()` (patch 0008) just logs "Busy in another cmd execution"
and returns, without rescheduling itself (there's a code comment
admitting this: `/* We should queue ourself here and remove the queuing
from process_cmd_resp */`). A command that gets queued while busy is only
serviced if some unrelated caller happens to trigger the workqueue again
before its own short timeout (500 ms for the WHY2025-added commands, 5 s
for the generic cfg80211 path) expires — otherwise it just silently times
out, never having been sent. This is the likely reason the three
WHY2025-added commands' timeouts fire "often" per their own comments, and
is a plausible root cause underneath the clobbering bug above. Fixing it
properly means having `esp_cmd_work` reschedule itself once `cur_cmd`
actually clears (e.g. from `process_cmd_resp()` and from the timeout
paths), which is a larger change to the command-channel's locking than
the ownership-check fix — deferred until it can be validated with a real
boot-cycle load test.

## BMI270 internal status check sometimes fails

After firmware upload, the chip's `INTERNAL_STATUS` register sometimes
doesn't read `MSG_INIT_OK`, and the driver returns -ENODEV. Boot
continues fine; sensor isn't enumerated on those cycles.

This surfaced with the 6.12 → 6.18 bump but is **not** an upstream
regression: the `drivers/iio/imu/bmi270/` driver first shipped in
v6.13, and its firmware-load/status-check path is unchanged from v6.13
through current mainline — a single unmasked `INTERNAL_STATUS` read
after a fixed 140–160 ms sleep, with no poll loop and no retry of the
upload. The 6.12-era build masked one-shot init failures behind an
`S05bmi270` sysfs-rebind init script that was dropped from the overlay
when the in-kernel retry-trigger (patch 0009) replaced it — but patch
0009 only retries `request_firmware()` -ENOENT, not a failed
`INTERNAL_STATUS` check, which is terminal.

The upstream check is also over-strict: it compares the whole register
against `MSG_INIT_OK` (0x01) instead of masking with
`BMI270_INTERNAL_STATUS_MSG_MSK` (defined but unused upstream), so any
set error/reserved bit fails init even when the message field reads
INIT_OK. The 8 KB config upload also runs over a ~100 kHz bit-banged
I²C bus with weak ~45 kΩ internal pull-ups, leaving little timing
margin.

**Fix shipped (pending hardware verification):** patch 0013 replaces
the single read with a masked poll (20 ms steps, 500 ms ceiling) and,
on timeout, soft-resets the chip (CMD 0xB6) and re-uploads, up to 3
attempts, logging the raw `internal_status` byte per failed attempt.
To verify: ~50 cold boots via a loop around `tools/bootcap.py`,
requiring `iio:device1` to enumerate on every cycle; the logged status
byte on any failing attempt discriminates the underlying cause
(0x00/0x04 = slow init, 0x02 = corrupted upload, high bits = was the
over-strict compare).

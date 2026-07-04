# Known issues

## Silent kernel wedge under sustained fork+exec churn

Reproducer (after login, badge boots cleanly):

```sh
(while :; do /bin/true; done) &
```

Kernel goes silent within seconds — no Oops, no `DETECT_HUNG_TASK`
output, no panic. UART input is also dead, so `tools/loadtest.py`'s
post-wedge state-capture commands return nothing. Recovery requires a
deep esptool reset. (With the patch 0019 MWDT — unverified on hardware
— a wedge should instead auto-reset within 30 s; see RUNTIME-WEDGE.md.
A ramoops log across that reset was attempted but pstore isn't
NOMMU-safe — see patch 0020.)

### Root cause (reframed 2026-07-04 by direct hardware experiment)

**The old "NOMMU FLAT exec alloc/free" hypothesis below is FALSIFIED.**
A controlled experiment on hardware (see the discriminator campaign in
`docs/RUNTIME-WEDGE.md`) established:

- A **pure shell-builtin busy loop** — `(while :; do :; done) &`, zero
  fork, zero exec, zero mmap, zero signals — **wedges the kernel** the
  same way `/bin/true` does. So the trigger is not exec, not the FLAT
  loader, not the NOMMU contiguous-block allocator, not the app_pool.
- But that same busy loop **run in isolation** (nothing else runnable)
  **never wedges**, even after minutes. The wedge only appears when a
  **second runnable task** exists (a heartbeat that `echo`/`sleep`s, or
  the fork/exec/exit churn of `/bin/true`) — i.e. when the workload
  causes **context switches** interleaved with the periodic tick.
- At the wedge, the ESP32-P4 ROM's watchdog-reset banner reports
  `Core0 Saved PC` in the **userspace** address range (>25 MB above the
  0x48000000 kernel base, well past `_end`). So the CPU is spinning in a
  userspace loop, **never being preempted** — the SYSTIMER interrupt has
  **stopped being delivered to the core**. The scheduler never runs, the
  kernel-fed MWDT (patch 0019) stops being fed, and the chip resets
  ~seconds later. That reset banner is the ground-truth wedge signal
  (host-sleep-immune — only a real kernel wedge stops the watchdog feed).
- Probing the console during the wedge window gets **no response on any
  input** (UART RX is a different, level-triggered CLIC slot), so it is
  not the timer slot alone that dies — **all** interrupt delivery stops.

So the wedge is a **CLIC-level interrupt-delivery failure under
concurrency**: after some interleaving of the timer interrupt with
context switches / other interrupts, the core stops taking *any*
interrupt while running userspace. The prime remaining suspect is the
CLIC running interrupt-level (`mintstatus.mil`) getting stuck at max
(all slots are level 7 with NLBITS=3, so nothing can preempt once `mil`
is stuck high), i.e. a bug in the CLIC v0.9 `mcause` entry/exit shim's
`mpil` handling in `arch/riscv/kernel/entry.S` (patch 0001) rather than
anywhere in mm/exec/signal.

### What's been ruled out (all hardware-tested)
- **Memory exhaustion / the NOMMU allocator / FLAT loader / app_pool.**
  A no-alloc, no-exec busy loop wedges just as hard (above).
- **`idle=poll` / WFI.** A 100%-CPU busy loop never enters the idle
  loop, yet wedges — the idle path is not involved.
- **Signal delivery (SIGCHLD), exec-time cache flushes, timer re-arm
  `-ETIME`.** Patches 0014/0015/0016 shipped and hardware-tested
  2026-07-04: none fixed it (see below).
- **The SYSTIMER being edge- vs level-triggered.** Patch 0022 switched
  it to level; still wedges (below).
- **A task resuming in userspace with interrupts masked (MIE=0).** Patch
  0023 forces `MPIE=1` on every return to userspace; still wedges, and
  all interrupts (not just the timer) stay dead — so the failure is at
  the CLIC delivery level, above per-task MIE.
- **Sensorpanel / file-content / UART-volume specific.** Long since
  ruled out: grep loops, `/bin/true`, and the pure builtin loop all
  wedge; single-shot commands survive.

**Boot-window frequency note (2026-07-04, transcript-audited):** across
~20 boots in one session, **3 verified in-stream MWDT resets** hit
~20–30 s after boot with only login-level activity (single open serial
session: login, one `ps` command echoed, then the ROM's
`HP_SYS_HP_WDT_RESET` banner arrived on the same port — the reset
reason is printed by mask ROM and cannot be faked host-side; an
RTS/port-open reset reads `rst:0x1 (POWERON)` instead). One further
incident was a **console-only freeze** (output stopped at ~3.7 s
uptime, no WDT reset ≥76 s — kernel alive and feeding, so this is the
older "boot freeze" class, not the wedge). A 5-boot no-interaction
loop was clean. So: ~3/20 with light interaction vs the historical
~1/30 — the deferred-probe window (esp-hosted init + pwm-backlight
probes + rcS) plus a little shell activity is a danger zone for the
same CLIC-delivery bug, while idle/normal boots look stable. Practical
consequences: interactive serial work right after login is unreliable
(do it fast, or idle ~45 s first), and boot-time reliability claims
need a fresh `freezetest.py` campaign against a current build.

### What it scales with
**The presence of concurrent context switches, not fork+exec rate.**
`/bin/true` (heavy fork/exec) wedges in ~35–90 s; a builtin busy loop
plus a once-per-5 s heartbeat wedges in ~25 s; the busy loop *alone*
does not wedge at all. Wall-time and "a second runnable task" matter;
exec rate does not (a 100×-higher fork rate does not wedge 100× faster).

### Diagnostic ceiling / next step
No Oops because nothing traps — the CPU just spins in userspace with
interrupts silently un-delivered. The ROM `Saved PC` localises it to
userspace (confirming "timer stopped", not "trap loop"). The definitive
next step is to record the SYSTIMER + CLIC register state (`mintstatus`,
per-slot `INTIE`/`INTIP`, `ST_CONF`/target) into a reserved,
`no-map` PSRAM region from the tick path and read it back after the
watchdog reset — see `docs/RUNTIME-WEDGE.md`.

### Candidate fixes tried (2026-07) — did NOT fix the wedge

A code audit found three independent mechanisms that all match the
wedge's phenomenology (scales with fork+exec rate, silent, timer IRQ
stops). Each got a hardening patch (0014–0016). **Hardware-tested
2026-07-04: the wedge still occurs** — `(while :; do /bin/true; done) &`
wedged the kernel after ~15 s of churn (heartbeat alive to uptime ~31 s,
then silent), same signature as before. So none of these three is the
(sole) root cause. The patches are kept as legitimate hardening (each
fixes a real latent bug) but are no longer wedge candidates. The one
thing that changed for the better: the patch 0019 MWDT auto-rebooted the
badge ~30 s after the wedge instead of a dead hang — see below. The
three mechanisms, for the record:

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

**Watchdog outcome (patch 0019), hardware-confirmed 2026-07-04:** when
the wedge hit, the kernel-fed MWDT stopped being fed (scheduler dead)
and reset the badge ~30 s later — a clean auto-reboot instead of the old
dead hang needing a deep esptool reset. The wedge is still unsolved, but
it is now *survivable*: soak/repro campaigns no longer need manual
recovery between hangs.

**Two further fixes tried and hardware-tested 2026-07-04 — also did NOT
fix the wedge** (kept as defensible hardening, honest about status):

- **Patch 0022 — level-trigger the SYSTIMER CLIC slot.** The SYSTIMER
  was the *only* edge-triggered CLIC slot (every other peripheral is
  `IRQ_TYPE_LEVEL_HIGH`); its `target0` output is a latched status bit,
  so level is the natural, self-healing fit and removes the entire
  "lost/coalesced edge kills the one-shot tick" failure class. Booted
  cleanly; the wedge survived slightly longer but still hit. Not the
  cause.
- **Patch 0023 — force `MPIE=1` on return to userspace.** Tested the
  hypothesis that a task resumes in userspace with interrupts masked.
  Still wedged, with *all* interrupts dead — proving the failure is at
  the CLIC delivery level, not per-task MIE (see "Root cause" above).

The pure-syscall no-exec discriminator that reframed this whole issue
*has now been run* (it wedges — exonerating exec/mm/pool). The remaining
lead is the CLIC `mcause`/`mil` handling in `arch/riscv/kernel/entry.S`;
the concrete next step is register-state capture across the watchdog
reset (see `docs/RUNTIME-WEDGE.md`).

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

# Silent kernel wedge — investigation plan

This is the plan for closing out the silent runtime wedge described
in [`KNOWN-ISSUES.md`](KNOWN-ISSUES.md). Steps are listed in
ascending order of cost; each one independently moves the needle.

> **Update 2026-07:** step 2's code audit is done and produced three
> shipped candidate fixes — patches 0014 (signal-path `mcause`
> poisoning via the still-live `regs->cause = -1UL` site), 0015 (ROM
> cache-thunk IRQ exclusion + no more whole-L2 invalidate per exec),
> and 0016 (systimer `-ETIME` on missed oneshot arm + torn-read fix).
> See the patch table in `patches/linux/README.md`. **Run the reproducer
> against a build with these before investing in steps 3-6.** They are
> not yet built or hardware-verified.
>
> The "non-printk dead-man's-switch" this plan kept wishing for now
> exists: patch 0019 arms the TIMG0 MWDT from probe (kernel-fed — feeds
> stop when the scheduler dies, chip hard-resets in ≤30 s; no more deep
> esptool reset, so unattended soak campaigns work). `reboot` also
> works now (MWDT restart handler), and PANIC_TIMEOUT=5 turns every
> oops into a clean reboot instead of a hang.
>
> A ramoops log to preserve the pre-wedge console tail across that
> reset was attempted (patch 0020) but backed out: `pstore`/
> `persistent_ram` maps its region with `vmap()`, which BUGs on NOMMU
> and panicked at boot (hardware-confirmed). Reviving it needs `no-map`
> on the reserved-memory node so `persistent_ram` takes the `ioremap`
> path instead — worth trying, since a persistent pre-wedge log is
> exactly what this investigation wants.

> **Update 2026-07-04 (hardware discriminator campaign — this reframes
> everything below).** Built in the OrbStack VM, flashed, and tested on
> the badge with a host-sleep-immune harness that classifies a wedge on
> the ROM's `HP_SYS_HP_WDT_RESET` banner (only a real kernel wedge stops
> feeding the MWDT). Results:
>
> - `(while :; do :; done) &` — **pure builtin loop, zero
>   fork/exec/mmap/signal — WEDGES** (WDT reset), same as `/bin/true`.
>   → steps 2 (`mm/nommu`+`binfmt_flat`), 3 (BMI/pwm), and 4 (app_pool
>   A/B) are all **moot**: the wedge has nothing to do with exec, the
>   FLAT loader, or the allocator.
> - The **same loop in isolation** (no second runnable task) **does not
>   wedge** — so step 1 (`idle=poll`) is also moot (a 100%-CPU loop never
>   idles). The trigger is **concurrent context switches** interleaved
>   with the periodic tick.
> - ROM `Core0 Saved PC` at every wedge is in **userspace** → the CPU is
>   spinning un-preempted; **interrupt delivery to the core has stopped**
>   (console input is dead too, so it's *all* interrupts, not just the
>   timer slot).
> - Fixes tried and **hardware-falsified**: 0022 (level-trigger the
>   SYSTIMER slot) and 0023 (force `MPIE=1` on user return). The latter
>   proves it is not per-task MIE — it's CLIC-level.
>
> **New leading hypothesis:** the CLIC v0.9 running interrupt-level
> (`mintstatus.mil`) gets stuck at max under a specific timer×context-
> switch interleaving, so no interrupt (all are level 7, NLBITS=3) can
> ever preempt again. That points at the `mcause` `mpil` save/restore in
> the entry/exit shim (`arch/riscv/kernel/entry.S`, patch 0001), NOT at
> mm/exec/signal.
>
> **Definitive next step (replaces steps 2–5 below):** capture the
> SYSTIMER + CLIC register state — `mintstatus`/`mil`, per-slot `INTIE`
> and `INTIP` for slots 17 (timer) and 18 (UART), `ST_CONF`+target — into
> a reserved `no-map` PSRAM region updated from the tick path (and/or a
> ring of the last N `mcause` values seen at trap entry), then read it
> back over `/dev/mem` after the watchdog reset. That directly answers
> "is `mil` stuck / is `INTIE` cleared / is the alarm disabled" and ends
> the guessing. (`no-map` is also what a revived ramoops needs — do both
> in one reserved region.) A `tools/` harness (`wedge_verify.py`) and the
> VM build+flash loop are already wired up for fast iteration.

> **Update 2026-07-04 (SMOKING GUN — patch 0025 wedge tracer).** The
> register-state capture proposed above is implemented (patch 0025:
> 128 KB `no-map` PSRAM carveout at `0x495e0000`; per-interrupt records
> of the RAW `mcause` CSR — which still holds `mpil[23:16]`, unlike the
> masked copy in pt_regs — plus `mepc`/`mintstatus`/`mstatus`, and
> per-tick snapshots of the memory-mapped CLIC threshold, slot 17/18
> byte quads and SYSTIMER alarm state; previous boot dumped to dmesg at
> early_initcall). Prerequisite discovered on the way: the boot shim ran
> a whole-PSRAM memtest every boot, wiping any persistent region —
> `CONFIG_SPIRAM_MEMTEST=n` now set in `linux-native`. PSRAM persistence
> across the MWDT reset is confirmed working.
>
> **First capture across a live wedge:** the last ~30 delivered
> interrupts are perfectly normal (timer ticks into kernel and into the
> busy-loop userspace, `mpil=0` throughout; per-tick threshold constant
> `0x1f`, slots enabled/level/ctl `0xFF`, alarm armed). Then the FINAL
> record ever delivered is:
>
>     cause=88ff0011  epc=0x497ce094 (userspace)  mstat MPP=U
>
> — a timer interrupt taken with **`mcause.mpil = 0xFF`**: the hardware
> says the core was running *userspace* at interrupt level 255, a state
> the exit shim makes impossible (it writes `mpil=0` before every
> `mret`). After that handler returned, no interrupt was ever delivered
> again; the MWDT reset ~30 s later. So: not the threshold register,
> not slot config, not the timer — the CLIC/core's *internal running
> level* gets stuck at 0xFF. Since every slot is level 0xFF and CLIC
> delivers only on `level > mil`, 255 ≯ 255 blocks everything forever.
>
> **Working theory:** an interrupt that races the preceding `mret`
> (pending back-to-back — which is why a second runnable task is
> needed) is accepted with a *mixed* context save: post-`mret`
> privilege/EPC (user) but pre-`mret` level (`0xFF`) — and the
> internal level-tracking never pops afterwards. ESP-IDF's exit path
> restores the identical `mcause.mpil` semantics and carries no visible
> workaround, so this smells like silicon/CLIC-v0.9 behaviour our
> higher interrupt+context-switch rate tickles far more often.
>
> **Fix directions (next steps):**
>  1. *Detect-and-study:* at dispatch, a non-nested entry with
>     `mpil != 0` is the anomaly one `mret` before death — log it and
>     experiment with active recovery (threshold pulse, slot
>     mask/unmask, etc.).
>  2. *Self-healing watchdog (architectural):* run all normal slots at
>     a mid level (e.g. INTCTL `0x7F` = level 3) and rewire the MWDT
>     stage 0 as a level-7 *interrupt* (stage 1 stays reset). A stuck
>     `mil=3` can then never block the level-7 dog; its handler repairs
>     the level via its own `mret` and feeds — converting the fatal
>     wedge into a ~2 s hiccup, with reset only as the second-stage
>     backstop.

> **Positive control PASSED (2026-07-04, closes the 0027 caveat).** With
> `/dev/watchdog` held open on a *healthy* system, the level-7 stage-0
> diagnostic interrupt fires and prints every round (mil=0xFF in-handler,
> ticks still advancing between rounds). The path is proven functional -
> so its silence during a real wedge is conclusive: **the wedge blocks
> all interrupt acceptance regardless of CLIC level.** A core-internal,
> level-independent latch from the mret race: silicon-erratum behaviour,
> not reachable by any software recovery tried. Remaining avenues: file
> with Espressif (trace captures + this doc), attempt to reproduce under
> ESP-IDF/FreeRTOS on a P4, or accept the MWDT reset as the shipping
> mitigation.

> **Avoidance falsified too (2026-07-04, patch 0029 — tried, measured,
> reverted).** The last untried software angle was avoidance: drain any
> *pending* CLIC interrupt from kernel context in `ret_from_exception`'s
> user path (trampoline mret to pop the still-raised running level —
> a plain MIE window can't accept an equal-level interrupt — then a
> short MIE window; rescan ≤8 rounds) so the final mret never executes
> with an interrupt pending. Hardware result, two-part:
>
> 1. **The wedge is unchanged.** `/bin/true` churn wedged in ~35–65 s
>    (baseline 35–90 s); a `wifi-connect` run wedged during `wlan0`
>    bring-up. WDT reset, same signature. (Bringing `wlan0` up turned
>    out to be the most reliable wedge trigger of all — since resolved —
>    `ipv6.disable=1`, see KNOWN-ISSUES "Resolved".)
> 2. **The drain provably never had work to do.** An instrumented build
>    (counters in the wedge-trace carveout, read back over `/dev/mem`)
>    showed 4048 executions of the drain scan across login + churn and
>    **zero** hits of pending+enabled (IP/IE bit positions verified
>    against Espressif's `clic_reg.h`). Makes sense in hindsight:
>    pendency can only accumulate in the few-hundred-ns IRQs-off exit
>    window, so "interrupt pending at the final mret" is ~once-per-
>    minutes rare — far too rare to be the trigger.
>
> Conclusion: the latch is triggered by an interrupt **arriving** in
> some exact cycle window during the mret-to-user sequence itself, not
> by pre-existing pendency. Asynchronous arrival cannot be gated by
> software (raising the CLIC threshold before mret deadlocks: nothing
> ever runs to lower it). With recovery (0026/0027) and avoidance
> (0029) both exhausted, the software story is closed: the MWDT stage-0
> reset (patch 0019) is the accepted shipping end-state. A real fix would have
> to come from silicon/Espressif, which is out of scope for this
> project — the auto-reset is good enough for a badge. 0029 was reverted
> — 16 MMIO reads per user return for a measured-zero benefit (the patch
> lives in git history at commit 54840c0).

> **RESOLVED BY SIDESTEP (2026-07-05, patch 0031): run userspace at
> M-mode — no privilege-dropping mrets, no latch.** Every piece of
> evidence above pointed at the *mret-to-user* transition: all Saved
> PCs in userspace, the mixed-context capture on an mret-to-user, and
> thousands of kernel-return mrets that never latched. The one lever
> never pulled was removing the privilege drop itself. The core does
> implement U-mode (the port's "no U-mode" assumption was wrong — the
> exit shim faithfully copied `status.MPP=U` into `mcause` and user
> tasks really ran at U), but nothing requires it: NOMMU userspace has
> no memory isolation anyway, PMP entries are unlocked, and the
> syscall path has handled ecall-from-M since bring-up. Patch 0031
> forces `mcause.MPP=M` on the exit shim's user-return branch and
> derives PT_STATUS's virtual privilege from the SCRATCH invariant at
> entry (hardware MPP now always reads M), so `user_mode()` semantics
> are unchanged.
>
> Hardware results (2026-07-05): the builtin-loop churn reproducer
> (baseline wedge ~25 s) ran **15 min clean**; `/bin/true` fork+exec
> churn (baseline 35–90 s) ran **12 min clean**; first-try boot success
> jumped to **29/30 = 96.7%** (p = 3×10⁻³ vs the 73.3% 0030 kernel,
> p = 2×10⁻⁴ vs the 60.5% baseline) with both historical freeze lines
> at zero. Full functional smoke passes. The MWDT (0019) stays armed
> as the backstop for whatever remains (one 1/30 freeze at an earlier,
> unrelated line was observed).
>
> One casualty of better data: the `wlan0 up` hang is **not this
> wedge**. It reproduces identically on the M-mode kernel, and its
> Saved PC is the same *kernel* address every time —
> `detach_if_pending` (kernel/time/timer.c), i.e. an IRQs-off infinite
> loop walking a corrupted timer-wheel list, not a delivery latch. Since
> resolved with `ipv6.disable=1` — see KNOWN-ISSUES "Resolved".

## 1. Remove `idle=poll` from cmdline (cheap, ~5 min)

The current cmdline forces the CPU to spin in `cpu_idle_loop()`
instead of using WFI. That was an early workaround when CLIC behaviour
under WFI was unclear. Now that the CLIC driver is stable, remove
`idle=poll` from `patches/linux/kernel.config`'s `CONFIG_CMDLINE` and
test the reproducer:

```sh
(while :; do /bin/true; done) &
```

If WFI-based idle path doesn't trigger the wedge, ship it. If the
kernel hangs at first idle, revert (the CLIC — a v0.9-spec
implementation — may still mishandle WFI wake-up on this rev v1.0
silicon).

## 2. Audit `mm/nommu.c` and `fs/binfmt_flat.c` for races (1–2 hr)

The wedge is in alloc/free/relocate during fork+exec on RV32 NOMMU.
Code-read these two files focused on:

  - Locks held across allocator calls.
  - Reentrancy in the FLAT relocation loop (we have a known issue
    where uClibc stdio loses relocations through elf2flt — that's
    binary-side; the kernel side is allocator + relocation patcher).
  - Memory-barrier requirements for PSRAM cache flushes around
    page tables / vma trees.

Cross-check against `linux-riscv@lists.infradead.org` archives for
similar reports. Cheap to do; either finds a known fix or rules it
out.

## 3. Disable BMI270 retry-trigger and pwm-c6 first-apply skip (1 hr)

These ship in patches 0009 (BMI270 retry-trigger) and 0011 (pwm-c6
first-apply skip) and fire from `late_initcall` /
deferred_probe_work. They might keep state alive that interacts with
sustained fork churn. Run the reproducer with each reverted (one at
a time) to confirm they're not contributing.

## 4. Pool-toggle A/B test (half-day)

The contiguous `linux,nommu-userspace-pool` is already implemented:
patch 0010 hooks it into `do_mmap_private`, and patch 0012's DTS
reserves 10 MB at `0x49600000` (`app_pool`), so the reproducer's
fork+exec allocations already run through the gen_pool path before
falling back to buddy. The discriminating experiment is therefore the
*inverse* of adding a pool: remove (or `status = "disabled"`) the
`app_pool` DT node — patch 0010's hook is an explicit no-op without
it — rebuild, and rerun the reproducer.

  - Wedge persists → the pool path is exonerated; suspect buddy /
    nommu core or a non-allocator cause.
  - Wedge disappears or changes timing → `mm/nommu_userspace_pool.c`
    and the `do_mmap_private` hook are implicated.

**Fix shipped (pending hardware verification):** the audit gap noted here
previously — patch 0010 only intercepting frees at the
`__put_nommu_region` full-free site — turned out to be worse than a
leak: `free_page_series()` on `memblock_reserve()`d pool memory is
refcount/state corruption, not just lost pages. Patch 0010 now also
guards `vmi_shrink_vma()` (partial unmap of an anonymous VMA — reachable
from an ordinary userspace `munmap()` of a sub-range, not just
binfmt_flat's whole-region unmaps) and `do_mmap_private()`'s
`error_free:` path (failed `kernel_read()` on a private file mapping).
Both were verified by fetching real upstream Linux 6.18.35 `mm/nommu.c`
and `lib/genalloc.c` and round-trip-applying the new hunks with zero
fuzz; `gen_pool`'s bitmap-based allocator was confirmed to support a
partial free of a sub-range safely. Not yet verified on hardware.

## 5. `CONFIG_PROVE_LOCKING=y` (1 hr)

Heavy. Likely overflows the 6.5 MB kernel partition; would need to
disable other things (KALLSYMS is already off, DETECT_HUNG_TASK is
already on).

If it fits and fires during the reproducer, points directly at the
lock inversion. If it fits and stays silent, the wedge isn't a
classic AB/BA deadlock.

## 6. JTAG / SWD investigation (needs hardware: ESP-Prog ~€10)

Connect to the badge's JTAG pads. When wedged, halt CPU and inspect
PC, registers, memory. Only path to ground-truth if 1–5 don't crack
it. Big up-front investment but unblocks every future kernel-debug
task on this board.

## 7. Upstream report (~30 min)

Even without a fix, a well-formed bug report to
`linux-riscv@lists.infradead.org` with the minimal reproducer and the
characterisation summary may surface someone who's seen it. Also
relevant for the FOSDEM-talk RFC angle.

## Recommended order

**1 → 7 → 2 → 4 → 6.** Skip 3 unless 1–2 produce nothing. Skip 5
unless the partition headroom permits it.

## Tools available

  - `tools/loadtest.py` — sensorpanel + heartbeat reproducer (~19 min wedge)
  - `tools/greptest.py` — grep-loop reproducer (~5–25 s wedge)
  - `tools/grepbisect.py` — bisects which subtree triggers (TICK heartbeat detector)
  - Simpler: `(while :; do /bin/true; done) &` after login (seconds)
  - `drivers/misc/esp32p4-watchdog-blink.c` — kernel kthread that
    pr_alerts every 5 s and toggles backlight via cmd_set_backlight.
    **Not in this repo** — the source was lost in the 32→12 patch
    squash; only the commented-out `obj-y` hook in patch 0005's
    `drivers/misc/Makefile` hunk survives. Needs to be rewritten
    before use. Distinguishes "scheduler dead" from "printk dead"
    from "SPI dead".

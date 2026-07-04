# Silent kernel wedge — investigation plan

This is the plan for closing out the silent runtime wedge described
in [`KNOWN-ISSUES.md`](KNOWN-ISSUES.md). Steps are listed in
ascending order of cost; each one independently moves the needle.

> **Update 2026-07:** step 2's code audit is done and produced three
> shipped candidate fixes — patches 0014 (signal-path `mcause`
> poisoning via the still-live `regs->cause = -1UL` site), 0015 (ROM
> cache-thunk IRQ exclusion + no more whole-L2 invalidate per exec),
> and 0016 (systimer `-ETIME` on missed oneshot arm + torn-read fix).
> See KNOWN-ISSUES "Candidate fixes shipped". **Run the reproducer
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

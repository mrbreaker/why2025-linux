# Wedge-investigation diagnostic patches (NOT in the shipping build)

These four patches are the instruments from the silent-CLIC-wedge
investigation (see [`../../docs/RUNTIME-WEDGE.md`](../../docs/RUNTIME-WEDGE.md)).
The investigation is **concluded** — the wedge is a level-independent,
core-internal CLIC latch on an `mret`-to-user interrupt race (silicon
erratum), with the MWDT stage-0 auto-reset (shipping patch `0019`) as
the mitigation. These patches did their job and are kept here for any
future wedge work.

**They are deliberately outside `patches/linux/`, so Buildroot's
`BR2_LINUX_KERNEL_PATCH` never applies them.** The shipping series is
`patches/linux/0001-0024`. Keeping the tracer in a shipping build is
pure cost: patch 0025 does a PSRAM write + ROM cache-flush thunk on
*every* interrupt dispatch.

An A/B on hardware (2026-07-04) confirmed the tracer is **not** the
cause of any wedge — with these applied the boot-freeze rate was
~25–33% (scattered), without them ~40% (concentrated at the
display+backlight bring-up); removing them only changes *where* the
wedge lands, not whether it happens. See the boot-reliability campaign
in [`../../docs/KNOWN-ISSUES.md`](../../docs/KNOWN-ISSUES.md).

## The patches

| # | File | What it adds |
|---|------|--------------|
| 0025 | `esp32p4-wedge-trace.patch` | PSRAM-persistent per-interrupt forensics: raw `mcause`/`mepc`/`mintstatus` on every CLIC dispatch + per-tick CLIC/SYSTIMER snapshots in a 128 KB `no-map` carveout at `0x495e0000`, dumped to dmesg on the next boot. Captured the `mpil=0xFF` smoking gun. Needs the boot shim built with `CONFIG_SPIRAM_MEMTEST=n` or the region is wiped each boot. |
| 0026 | `esp32p4-wedge-trace-mpil-repair.patch` | Dispatch-time detection of `mcause.mpil != 0` (the anomaly one `mret` before death) + an in-handler repair ladder (threshold pulse, INTIE toggle). Detection works; repair does not unstick the latch. |
| 0027 | `esp32p4-wdt-level7-diag-interrupt.patch` | CLIC level partition (normal slots level 3, MWDT slot level 7) + MWDT stage-0 rewired as a level-7 diagnostic interrupt. Proved the block is level-independent (the level-7 IRQ does not deliver into a wedge, though it fires on a healthy system). |
| 0028 | `esp32p4-wdt-stage0-reset-default.patch` | Reverts 0027's diagnostic `interrupts-extended` DTS line so the MWDT falls back to plain stage-0 reset. Only meaningful on top of 0027. |

## Re-enabling them

They apply `-p1` on top of the current shipping series (base: Linux
6.18.35 + `patches/linux/0001-0024`). To run a diagnostic build:

```bash
cp patches/linux-diagnostics/00*.patch patches/linux/   # 0025-0028
# rebuild the kernel (see BUILDING.md); also set CONFIG_SPIRAM_MEMTEST=n
# in linux-native/ for 0025's PSRAM persistence to survive the reset
```

Remove them from `patches/linux/` again before shipping.

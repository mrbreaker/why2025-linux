# Linux kernel patches

Patch series against `linux-6.18.35` (LTS), applied by Buildroot via
`BR2_LINUX_KERNEL_PATCH=patches/linux`. Apply order is filename-sort.

The series is squashed: each patch is a single coherent topic that
ends in the final shipped state, rather than the incremental
development history. There were 32 in-flight patches during bring-up;
they were consolidated into 12 topic patches at publication time, and
new work lands as additional topic patches on top (0013+).

## Series

| #    | Patch | What it does |
|------|-------|--------------|
| 0001 | `riscv-esp32p4-baseline.patch` | RV32 ESP32-P4 platform support: CLIC v0.9 irqchip (`drivers/irqchip/irq-esp32p4-clic.c`), SYSTIMER clocksource (`drivers/clocksource/timer-esp32p4-systimer.c`), `esp32_uart` console driver, ESP32-P4 cache ops (`drivers/cache/esp32p4_cache.c`, RISCV_NONSTANDARD_CACHE_OPS), signal-delivery fixes in `arch/riscv/kernel/entry.S` for CLIC's `MINHV` bit, ARCH defconfig. |
| 0002 | `gpio-esp32p4.patch` | `gpio-esp32p4` driver: bank-aware GPIO + IO_MUX programming, irq_chip routed via INTMTX, set_config for FUN_WPU pull-up. Bank 0 covers GPIO 0..31, bank 1 covers 32..63. |
| 0003 | `drm-mipi-dsi-esp32p4.patch` | DRM/MIPI-DSI host glue (`drivers/gpu/drm/espressif/dw-mipi-dsi-esp32p4.c`) wrapping the Synopsys DW MIPI-DSI core with ESP32-P4 bridge + DPI + DW-GDMA. fbcon defio: `drm_gem_fb_create_with_dirty` enables the dirty-rect path so userspace `write()`/`pwrite()` notifies a copying `pipe_update`. Includes the upstream `dw-mipi-dsi.c` tweak that exposes the panel-bridge mode_config funcs to glue drivers. |
| 0004 | `drm-panel-st7703-bono.patch` | Sitronix ST7703 panel definition for the WHY2025 BONO panel: 720×720 RGB565, 2 lanes @ 1000 Mbps, locked-MV-bit (rotation done in DRM glue blit). |
| 0005 | `misc-esp32-c6-kick.patch` | Minimal driver to release the ESP32-C6 from reset at boot (P4 reset = GPIO 12 active-low, boot-mode strap = GPIO 13). Requests the reset line with `GPIOD_OUT_LOW` so the C6 comes up deasserted (released). |
| 0006 | `input-tca8418-keypad-polling.patch` | Polling fallback for the TCA8418 keypad scanner (the WHY2025 badge does NOT route the chip's NOT/INT line, so the upstream IRQ-only path fails). When `client->irq <= 0` the driver registers via `input_setup_polling` instead. |
| 0007 | `mmc-dw_mmc-esp32p4-fixes.patch` | dw_mmc: restore generic multi-slot support via `snps,slot-id` DT property (mainline dropped it) and a cached-descriptor-ring IDMAC fallback for ESP32-P4 cache-coherency oddities. The badge's own microSD is on the default slot 0 and its C6 link is SPI, so it doesn't set `snps,slot-id` — this is generic plumbing, not badge-specific. |
| 0008 | `net-wireless-esp-hosted-ng-spi.patch` | esp-hosted-NG host driver, ported in-tree (mainline-style `spi_driver` with DT match). SPI variant (vs SDIO upstream) with custom HANDSHAKE/DATA_READY pin Kconfig, advisory-cmd HZ/2 timeout for `cmd_set_ip_address` / `cmd_set_mcast_mac_list` (these are best-effort in client mode; default 5 s timeout × 3 NETDEV events per DHCP lease produced 15 s of dmesg noise). |
| 0009 | `iio-imu-bmi270-retry-trigger.patch` | BMI270: on `request_firmware -ENOENT` schedule a one-shot `delayed_work` at +5 s that calls `driver_deferred_probe_trigger()` to retry. Needed because /lib/firmware ships in the squashfs root partition which is mounted ~3.4 s in, but built-in IIO drivers probe ~3.2 s. Bounded retry (6 attempts) so a permanently-absent firmware blob doesn't loop. |
| 0010 | `mm-nommu-userspace-pool.patch` | New `linux,nommu-userspace-pool` reserved-memory binding + a NOMMU mmap pool routed through `do_mmap_private`. Reserves 10 MB at 0x49600000 for FLAT-binary `mmap-anon`, so fbdoom -mb 6 has a contiguous 6 MB allocation even when the buddy heap is fragmented. |
| 0011 | `pwm-esp-hosted-c6-backlight.patch` | `pwm-esp-hosted-c6` PWM controller fronting the ESP32-C6's LEDC channel via SPI (CMD_SET_BACKLIGHT slot 32 over esp-hosted-NG). Skips the very first `.apply()` at probe time to avoid an SPI race with esp-hosted-NG's bluetooth init that otherwise wedges ~15 % of cold boots. The C6 slave's `BL_DISPLAY_INITIAL_DUTY=76` covers the missed write. |
| 0012 | `dts-esp32p4-why2025-badge.patch` | DTS for the WHY2025 badge: clocks + UART, GPIO banks (matrix-bank0 + bank1), CLIC, SYSTIMER, dw_mmc + slot-1 routing, i2c-gpio bus 0 with TCA8418 keypad + BME680 + BMI270, DSI host + ST7703 panel, esp-hosted-NG SPI device, pwm-c6 + pwm-backlight, mtd-rom `flash@deadbeef` for the squashfs rootfs (boot shim patches the placeholder), nommu userspace pool reservation. |
| 0013 | `iio-imu-bmi270-init-status-retry.patch` | BMI270: harden the config-load `INTERNAL_STATUS` check (upstream does a single unmasked read after a fixed 140 ms sleep). Poll the masked `message` field (`MSG_MSK`, 20 ms × up to 500 ms) per the datasheet's "wait until init_ok" procedure, and on timeout soft-reset (CMD 0xB6) and re-upload, up to 3 attempts. Logs the raw status byte on each failed attempt. Fixes the intermittent boot-time `-ENODEV` in `docs/KNOWN-ISSUES.md`. |

## Build configs included here

  - `kernel.config` — canonical Linux 6.18.35 config. Notable knobs:
    `KALLSYMS=n` (kernel partition is 6.5 MB, KALLSYMS+ALL overflows it),
    `DETECT_HUNG_TASK=y` (30 s timeout), `MAGIC_SYSRQ=y` (incl.
    `MAGIC_SYSRQ_SERIAL`), `WQ_WATCHDOG=y`, `DYNAMIC_DEBUG=y`,
    `INITRAMFS_SOURCE=` (squashfs root), `CMDLINE_FORCE=y` with
    `idle=poll`.

The Buildroot defconfig lives at `../../configs/why2025_defconfig`.

## Squashing history

The patches above were collapsed from a 32-patch development series.
The full final-state set was verified against the development tree:
both produce byte-identical output for every touched file in
`linux-6.18.26` (the base at the time of the squash; the series has
since been re-verified to apply cleanly — no fuzz, no offsets — on
`linux-6.18.35`). If you want to see the bring-up step-by-step, look
at git history before the squash commit; subsequent work should land
as new topic patches against the current series rather than as
incremental fixes.

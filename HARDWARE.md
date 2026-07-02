# WHY2025 badge hardware reference

The badge is the WHY2025 hacker camp badge by [Badge.team]. This document
captures the pin map and peripheral wiring relevant to the native Linux port.

[Badge.team]: https://why2025.org/

## SoC

  - **ESP32-P4** rev v1.0
  - Dual HP core RV32IMAFC + LP core RV32IMAC at up to 360 MHz
  - **32 MB HEX PSRAM** at 200 MHz DDR (10 GB/s, 16-bit bus)
  - **16 MB SPI flash** in DIO mode

Linux runs on **one** HP core in M-mode, NOMMU. The second HP core is unused.

## Coprocessor

  - **ESP32-C6** for Wi-Fi (station mode), Bluetooth LE, and backlight PWM.
  - Connected to the P4 over GPSPI2 (mode 2, ~26 MHz) plus two control lines.
    Same physical pins originally wired for SDIO; routed through the GPIO
    matrix on both sides.
  - C6 reset on P4 GPIO 12 (active-low); boot-mode strap on P4 GPIO 13.

## Pin map (P4 ↔ C6 over SPI)

| Signal       | P4 GPIO | C6 GPIO |
|--------------|---------|---------|
| SCK          | 32      | 19      |
| MOSI         | 33      | 18      |
| MISO         | 28      | 20      |
| CS           | 31      | 21      |
| HANDSHAKE    | 30      | 22      |
| DATA_READY   | 29      | 23      |

## Display

  - WHY2025 BONO panel: 720×720 RGB565 over MIPI-DSI, 2 lanes at 1000 Mbps.
  - Sitronix ST7703 driver, video mode, 61 Hz.
  - Panel's MV bit is locked, so the kernel does a 90° CCW rotation in
    `pipe_update`. There's also a +16-px vertical shift compensated in the blit.
  - DPI clock 48 MHz (PLL_F240M / 5).
  - Panel reset on P4 GPIO 17 (active high per ST7703 spec).

## Keypad

  - **TI TCA8418** I²C keypad scanner at addr `0x34`.
  - SDA = P4 GPIO 18, SCL = P4 GPIO 20 (bit-banged i2c-gpio bus 0).
  - INT line is **not routed** to the P4 — driver polls every 50 ms.
  - Fast press+release inside one 50 ms poll window is not dropped: both
    events queue in the chip's 10-entry hardware key FIFO and are drained
    together on the next tick. Worst-case added key latency is ~50 ms.
  - 8 rows × 10 cols matrix. PS-symbol keys (square/triangle/cross/circle/
    cloud/diamond) currently mapped to F1..F6 — see DTS for full keymap.

## Sensors

Both share the i2c-gpio bus 0:

  - **BME680/688/690** at addr `0x76` (chip ID 0x61). Mainline `bosch,bme680`.
    Pressure reads ~227 kPa instead of ~100 kPa due to BME680 vs BME688/690
    calibration coefficient differences (cosmetic).
  - **BMI270** at addr `0x69` (chip ID 0x24). Needs `bmi270-init-data.fw`
    (8 KB) shipped via rootfs overlay. Driver delays init via a delayed_work
    that triggers `driver_deferred_probe_trigger()` 5 s after the first
    `request_firmware -ENOENT` so the upload happens after squashfs mounts.

## Storage

  - **microSD** on `dw_mmc` slot 0 (mmcblk0). Multi-slot support patched
    back in via `snps,slot-id`. IDMAC works after `riscv_noncoherent_*`
    cache ops + ESP32-P4 ROM helpers.

## Power

  - PMIC-controlled. **No reset button** — hold the power button while
    plugging USB to start.
  - Two USB-C ports: side reaches the P4 (CH340, `/dev/cu.wchusbserial10`
    on macOS), back reaches the C6 (CH334 hub) for slave reflash.

## Flash layout

| Offset    | Size   | Content                              |
|-----------|--------|--------------------------------------|
| 0x002000  | 24 KB  | ESP-IDF bootloader                   |
| 0x008000  | 4 KB   | Partition table                      |
| 0x009000  | 24 KB  | NVS                                  |
| 0x00f000  | 4 KB   | PHY init                             |
| 0x010000  | 0.5 MB | Boot shim (linux-native.bin)         |
| 0x090000  | 6.5 MB | Linux kernel `Image`                 |
| 0x710000  | 8 MB   | rootfs (squashfs, must be POW2)      |
| 0xF10000  | 64 KB  | Device tree blob                     |

## Memory layout (PSRAM)

```
0x48000000              kernel + RAM
0x49600000  +10 MB      NOMMU userspace pool (FLAT exec backing)
0x49ffffff
```

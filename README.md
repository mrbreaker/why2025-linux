# why2025-linux

I picked up a WHY2025 hacker camp badge and used it as an excuse to 
learn more about Linux kernel internals and microcontrollers boot. This
started as a test to see if it was possible to boot native Linux on this,
but it got a bit out of hand. There's a working DSI panel, Wi-Fi, Bluetooth,
the IMU/environmental sensors, fbDOOM, and a small kernel patch series.
I won't pretend it's upstream-ready but it's enough for a proof of concept.

As I said, it's a proof of concept, so don't assume it's a stable build. 
There's a documented runtime issue under heavy fork+exec churn that I 
haven't solved, among other rough edges. It's definitely not battle-tested.

I worked closely with AI throughout this project (mostly Claude). As I said,
this was a learning experience never intended to publish. But I got too
excited about this project and its results not to share it with the world.

## What's running

Native RV32 NOMMU Linux 6.18.35 LTS on the ESP32-P4's HP core. The HP
core is a real RISC-V CPU (RV32IMAFC), so this isn't emulation. A
small ESP-IDF boot shim copies a flat `Image` from flash into 32 MB of
HEX PSRAM at `0x48000000` and jumps to it. The 8 MB rootfs is a
squashfs mounted via `mtd-rom` directly out of flash.

Boot to BusyBox login takes about 7 seconds. From there:

- 720×720 DSI panel with `fbcon`, full keypad mapped to evdev.
- Fn+Esc puts the badge display to sleep (display + keyboard
  backlights off, fb blanked, input grabbed); any key wakes it.
- BME680 + BMI270 readable via `iio:device*`.
- Wi-Fi via `wifi-connect <ssid> <psk>`, Bluetooth LE scan via a
  small custom HCI helper.
- microSD card readable + VFAT-mountable.
- fbDOOM playable on the panel (`-mb 6`).
- ~97% cold-boot reliability; the residual ~1/30 boot freeze is a
  known issue (see [`docs/KNOWN-ISSUES.md`](docs/KNOWN-ISSUES.md)).

## Screenshots

![Boot console on the panel](screenshots/boot.jpg)

![fbDOOM running natively](screenshots/fbdoom.jpg)

![sensorpanel, live BME680 + BMI270 readings](screenshots/sensorpanel.jpg)

## Repository layout

```
configs/        Saved Buildroot defconfig (Buildroot 2025.02.15 LTS)
linux-native/   ESP-IDF boot shim
patches/
  linux/        kernel patch series + kernel.config
  buildroot/    rootfs overlay + FLAT userspace utilities
  c6-slave/     patches against upstream esp-hosted-ng
tools/          pyserial test harnesses
docs/           known issues + investigation notes
```

For the technical side: what each patch does, how to build, the full
hardware reference, see: [`BUILDING.md`](BUILDING.md),
[`HARDWARE.md`](HARDWARE.md), and the per-directory READMEs.

## License

Mixed; see [`LICENSE`](LICENSE). Kernel patches under `patches/linux/`
are GPL-2.0. Boot shim and C6 slave are Apache-2.0 OR GPL-2.0 (matching
upstream ESP-IDF / esp-hosted-ng licensing). Tools are MIT.

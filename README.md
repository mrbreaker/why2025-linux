# why2025-linux

I picked up a WHY2025 hacker camp badge and used it as an excuse to 
learn more about Linux kernel internals and microcontrollers boot. This
started as a test to see if it was possible to boot native Linux on this,
but it got a bit out of hand. There's a working DSI panel, Wi-Fi, Bluetooth,
the IMU/environmental sensors, fbDOOM, and a small kernel patch series.
I won't pretend it's upstream-ready but it's enough for a proof of concept.

As I said, it's a proof of concept, so don't assume it's a stable
build — [`docs/KNOWN-ISSUES.md`](docs/KNOWN-ISSUES.md) has the rough
edges. It's definitely not battle-tested.

I worked closely with AI throughout this project (mostly Claude). As I said,
this was a learning experience never intended to publish. But I got too
excited about this project and its results not to share it with the world.

## What's running

Native RV32 NOMMU Linux 6.18.35 LTS on the ESP32-P4's HP core. The HP
core is a real RISC-V CPU (RV32IMAFC), so this isn't emulation. A
small ESP-IDF boot shim copies a flat `Image` from flash into 32 MB of
HEX PSRAM at `0x48000000` and jumps to it. The 8 MB rootfs is a
squashfs mounted via `mtd-rom` directly out of flash.

Boot to a login prompt on the panel takes about 5.5 seconds. From
there:

- 720×720 DSI panel with `fbcon`, full keypad on evdev. Fn+Esc sleeps
  the display, any key wakes it.
- Wi-Fi: `wifi-connect '<ssid>' '<psk>'` — associates, gets a DHCP
  lease, and offers to save the credentials to the SD card for
  auto-connect at boot (followed by an NTP time sync; the badge has no
  RTC).
- SSH, both directions: inbound key-only (drop an `ssh-ed25519` pubkey
  in `badge/ssh/authorized_keys` on the card), outbound
  `ssh`/`scp` (servers must offer an ed25519 host key).
- microSD auto-mounted at `/mnt/sd` (FAT32 + exFAT). `badge/` on the
  card persists Wi-Fi credentials, SSH keys, shell config, and an
  `rc.local` hook — the rootfs itself stays read-only squashfs.
- `launcher` — a keypad-driven menu on the panel (sensor panel, DOOM,
  Wi-Fi status, BLE advertise, backlight), so the badge is usable
  standalone without a serial cable.
- fbDOOM, from the menu or
  `fbdoom -iwad /usr/share/games/doom/doom1.wad -mb 4`.
- Bluetooth LE scan + advertise (`ble_scan`, `ble_adv` — broadcasts
  the badge's hostname so other badges can see it).
- busybox vi, colour prompt, shell history, and a login banner with
  quick-start hints.
- BME680 + BMI270 via `iio:device*`; WS2812B LED support (with kernel
  triggers) for the frontpanel addon.
- A hardware watchdog armed before anything fragile probes: a boot
  freeze costs one automatic retry, never a brick. First-try boot
  rates and the one remaining freeze class are tracked in
  [`docs/KNOWN-ISSUES.md`](docs/KNOWN-ISSUES.md).
- The nastiest find of the project — a silicon-level CLIC
  interrupt-delivery latch that wedged the kernel under multi-process
  churn — is root-caused and avoided (userspace runs at physical
  M-mode; [`docs/RUNTIME-WEDGE.md`](docs/RUNTIME-WEDGE.md) has the
  investigation).

## Installing

Grab `esp32p4.bin` and `esp32c6.bin` from
[Releases](https://github.com/mrbreaker/why2025-linux/releases) and
flash each chip on its own USB-C port (`pip install esptool`):

```sh
# side USB-C port (P4 — Linux)
esptool --chip esp32p4 -p /dev/<p4-port> -b 460800 write-flash 0x0 esp32p4.bin

# bottom USB-C port (C6 — Wi-Fi/BT coprocessor; only needed once)
esptool --chip esp32c6 -p /dev/<c6-port> -b 460800 write-flash 0x0 esp32c6.bin
```

Find the ports with `ls /dev/cu.*` (macOS) or `ls /dev/ttyUSB*
/dev/ttyACM*` (Linux); the P4 enumerates as a CH340, the C6 as
`usbmodem`/ACM. **Flash the P4 first, then the C6, then power-cycle
the badge** (unplug all cables). A C6 flashed while the factory P4
firmware is still running can be left stuck in download mode — screen
stays black; a power cycle or a C6 reflash recovers it. To build from
source instead: [`BUILDING.md`](BUILDING.md).

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

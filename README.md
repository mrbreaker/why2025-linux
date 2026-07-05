# why2025-linux

I picked up a WHY2025 hacker camp badge and used it as an excuse to 
learn more about Linux kernel internals and microcontrollers boot. This
started as a test to see if it was possible to boot native Linux on this,
but it got a bit out of hand. There's a working DSI panel, Wi-Fi, Bluetooth,
the IMU/environmental sensors, fbDOOM, and a small kernel patch series.
I won't pretend it's upstream-ready but it's enough for a proof of concept.

As I said, it's a proof of concept, so don't assume it's a stable build.
The nastiest bug — a silicon-level interrupt-delivery latch that wedged
the kernel under heavy fork+exec churn — is root-caused and avoided
(patch 0031 runs userspace at M-mode; `docs/RUNTIME-WEDGE.md` has the
full investigation), but other rough edges remain. It's definitely not
battle-tested.

I worked closely with AI throughout this project (mostly Claude). As I said,
this was a learning experience never intended to publish. But I got too
excited about this project and its results not to share it with the world.

## What's running

Native RV32 NOMMU Linux 6.18.35 LTS on the ESP32-P4's HP core. The HP
core is a real RISC-V CPU (RV32IMAFC), so this isn't emulation. A
small ESP-IDF boot shim copies a flat `Image` from flash into 32 MB of
HEX PSRAM at `0x48000000` and jumps to it. The 8 MB rootfs is a
squashfs mounted via `mtd-rom` directly out of flash.

Boot to a login prompt on the panel takes about 5.5 seconds
(hardware-measured median across 23 boots: fbcon at 5.54 s, full video
at 5.69 s — down from ~7.2 s/10 s before the 2026-07-05 display
bring-up rework). From there:

- 720×720 DSI panel with `fbcon`, full keypad mapped to evdev.
- Fn+Esc puts the badge display to sleep (display + keyboard
  backlights off, fb blanked, input grabbed); any key wakes it.
- BME680 + BMI270 readable via `iio:device*`.
- Frontpanel WS2812B RGB LEDs via
  `/sys/class/leds/rgb:indicator-{0..3}/` (`brightness` +
  `multi_intensity`), with kernel LED triggers enabled — `echo
  heartbeat > .../trigger` for a zero-userspace kernel-alive blinker,
  `netdev` for a Wi-Fi activity light.
- Bluetooth LE scan + advertise via small custom HCI helpers
  (`ble_scan`; `ble_adv &` broadcasts the badge's hostname so other
  badges' scans can see it). *(ble_adv shipped 2026-07-05, pending
  hardware verification)*
- Wi-Fi **working** (`wifi-connect "<ssid>" "<psk>"` → associate + DHCP
  + reach the internet), hardware-verified 2026-07-05. The old `wlan0
  up` hang was an IPv6 timer (`rs_timer`) corrupting the timer wheel,
  not the CLIC erratum; it is disabled with `ipv6.disable=1` on the
  kernel cmdline (IPv6 has no use on this badge). Some esp-hosted
  datapath lifecycle bugs remain latent — see
  [`docs/KNOWN-ISSUES.md`](docs/KNOWN-ISSUES.md).
- microSD auto-mounted at `/mnt/sd` at boot (FAT32 + exFAT); config
  persists in `badge/` on the card — `wifi.conf` auto-connect (followed
  by a one-shot NTP clock sync; the badge has no RTC), `profile.sh`,
  `rc.local`, SSH keys — while the rootfs itself stays read-only
  squashfs. Hot-insert: `mount /mnt/sd`. *(shipped 2026-07-05, pending
  hardware verification)*
- SSH via dropbear, both directions: inbound key-only (put an
  `ssh-ed25519` pubkey in `badge/ssh/authorized_keys` on the card) and
  outbound `ssh`/`dbclient`/`scp` from the badge (talks to servers
  with ed25519 host keys; `known_hosts` persists on the card). Server
  side hardware-verified 2026-07-05 (inetd listening); sessions
  end-to-end pending.
- busybox vi, colour prompt, shell history, sane `TERM` on the panel
  console, and a login banner with quick-start hints.
- `launcher` — a keypad-driven menu on the panel (sensor panel, DOOM,
  Wi-Fi status, BLE-advertise toggle, backlight), so the badge is
  usable standalone without a serial cable.
- fbDOOM playable on the panel (`-mb 6`), also launchable from the
  `launcher` menu. (Vendored via
  `patches/buildroot/buildroot-tree/0002` after the original local
  package was lost — fresh clones build it again as of 2026-07-05.)
- Boot reliability: the watchdog now guarantees the badge always
  reaches login (patch 0034 arms it before any fragile probe — no
  freeze is ever hard-dead), and the cold-boot killer is gone (patch
  0035: the Bluetooth HCI burst that wedged cable-plug boots at ~21 s
  is opt-in at runtime now; a post-fix hand campaign saw ~7/9 cold
  boots clean vs ~1/6 before). First-try rates measured 2026-07-05:
  warm resets 23/30 with the SD card inserted (the residual freezes
  are one pre-existing class at the SD partition-scan line, possibly
  card-correlated — see `docs/KNOWN-ISSUES.md`; the earlier 29/30 was
  measured with card state unrecorded), all failures
  watchdog-recovered. History: ~60% baseline, ~73% with patch 0030,
  ~97% with patch 0031.
- The CLIC interrupt-delivery latch that used to wedge the kernel
  under multi-process churn (a silicon-level erratum, root-caused at
  the register level) is **avoided as of patch 0031**: userspace runs
  at physical M-mode, so the privilege-dropping `mret` the latch needs
  never executes. Churn reproducers that wedged in under 90 s now run
  indefinitely; the watchdog stays armed as a backstop
  (see [`docs/RUNTIME-WEDGE.md`](docs/RUNTIME-WEDGE.md)).

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

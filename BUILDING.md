# Building why2025-linux

Three artifacts to build, in order:

  1. **Kernel + rootfs** via Buildroot 2025.02.4 (Linux 6.18.26 LTS).
  2. **Boot shim** via ESP-IDF v5.5.3 (the small ESP32-P4 application
     that loads the kernel into PSRAM and jumps to it).
  3. **C6 slave firmware** via ESP-IDF (the ESP32-C6 coprocessor that
     handles Wi-Fi, BT, and the backlight PWM over SPI).

You build (1) and (2) on a Linux host (Buildroot doesn't run on macOS).
This project's reference setup is an OrbStack VM on macOS, but any
Ubuntu / Debian box works. (3) builds the same way as any ESP-IDF
project.

## Prerequisites

  - Linux build host (Ubuntu 24.04 reference).
  - ESP-IDF v5.5.3, installed per
    https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32p4/get-started/.
    Set `IDF_PATH` and source `$IDF_PATH/export.sh`.
  - Buildroot 2025.02.4: `git clone -b 2025.02.4 https://gitlab.com/buildroot.org/buildroot`
  - Host-side flash tools (run from macOS where the badge is plugged in):
    `pip install esptool pyserial`
  - `tio` for serial console (optional but recommended).

### Building on macOS

Buildroot doesn't run on macOS, so you need a Linux environment. Two
common options:

  - **OrbStack** (https://orbstack.dev) — fast, lightweight Linux VMs
    with first-class filesystem sharing. Reference setup: `orb create
    ubuntu rv32dev`, then symlink `~/esp32p4` on the macOS side to
    `~/esp32p4` inside the VM so paths match in both places. ESP-IDF
    and Buildroot both install inside the VM. esptool runs from macOS
    (USB passthrough is unreliable on virtualised guests).

  - **Lima**, **Multipass**, **UTM**, or any other Linux VM. Same
    pattern: build inside Linux, flash from macOS where the badge is
    enumerated as `/dev/cu.wchusbserial10`.

Inside a VM you can prefix commands with `orb -m rv32dev bash -c '...'`
(OrbStack) or use `ssh` (Lima/UTM) for one-shot builds without entering
a shell.

## 0. One-time setup after cloning

Buildroot's `.config` (and Linux's `CONFIG_EXTRA_FIRMWARE_DIR`) only
accept absolute filesystem paths, so we can't ship them committed. The
two files that need patching are `configs/why2025_defconfig` and
`patches/linux/kernel.config`; both contain the literal token
`@WHY2025_LINUX@` wherever an absolute path to this repo is needed
(rootfs overlay, post-build hook, kernel patch dir, kernel custom
config, embedded firmware dir).

A helper substitutes that token with the repo's actual absolute path:

```bash
./setup-paths.sh
```

It replaces every `@WHY2025_LINUX@` with `$(pwd)`. Idempotent — safe to
re-run after a `git pull` or a fresh checkout. If you'd rather do it by
hand:

```bash
sed -i.bak "s|@WHY2025_LINUX@|$PWD|g" configs/why2025_defconfig patches/linux/kernel.config
```

You'll know it worked when
`grep -r '@WHY2025_LINUX@' configs patches/linux/*.config`
returns nothing.

## 1. Kernel + rootfs

```bash
cd /path/to/buildroot

# Easiest path: load the shipped defconfig (after running setup-paths.sh).
cp /path/to/why2025-linux/configs/why2025_defconfig .config
make olddefconfig
make -j$(nproc)
```

The shipped defconfig already wires `BR2_LINUX_KERNEL_PATCH`,
`BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE`, `BR2_ROOTFS_OVERLAY`, and
`BR2_ROOTFS_POST_BUILD_SCRIPT` to the right paths in this repo. If you
prefer to override at the make command line:

```bash
make BR2_LINUX_KERNEL_PATCH=/path/to/why2025-linux/patches/linux \
     BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=/path/to/why2025-linux/patches/linux/kernel.config \
     ...
```

Outputs land in `output/images/`:

  - `Image` — flat kernel image (~6.5 MB)
  - `rootfs.squashfs` — read-only root (~5 MB)
  - `output/build/linux-6.18.26/arch/riscv/boot/dts/espressif/esp32p4-why2025.dtb`

The buildroot overlay (`patches/buildroot/overlay/`) is applied
automatically if the buildroot config points at it. Post-build script in
`patches/buildroot/post-build.sh` chmods init scripts.

Faster rebuilds:
```bash
# Just the kernel after a patch change
make linux-rebuild all -j$(nproc)

# Re-apply kernel.config after editing it
make linux-reconfigure all -j$(nproc)
```

> **Note:** Buildroot's overlay rsync only *adds* files. If you remove
> something from `patches/buildroot/overlay/` (e.g. an init.d script),
> the file persists in `output/target/` from the previous build and
> ends up in the rootfs anyway. After shrinking the overlay run
> `rm -rf output/target` (or `make clean`) before the next build.

## 2. Boot shim

```bash
cd /path/to/why2025-linux/linux-native
. $IDF_PATH/export.sh
idf.py build
```

Output: `build/linux-native.bin` at flash offset `0x10000`.

The boot shim does:
  - Maps the rootfs partition into the IDF cache window.
  - Two-tier MMU prewalk (coarse 64 KB stride + fine 64 B stride for the
    last 64 KB to fix near-EOF squashfs metadata reads — see
    `docs/KNOWN-ISSUES.md`).
  - Patches the DTB's `flash@deadbeef` placeholder with the actual VA + size.
  - Jumps to `0x48000000`.

## 3. C6 slave firmware

The slave is a small fork of esp-hosted-ng v1.0.6. We don't carry the
full slave tree in this repo; instead, clone upstream and apply our
patches:

```bash
git clone -b release/ng-1.0.6 https://github.com/espressif/esp-hosted.git
cd esp-hosted/esp_hosted_ng/esp/esp_driver/network_adapter

for p in /path/to/why2025-linux/patches/c6-slave/00[0-9][0-9]-*.patch; do
    patch -p1 -i "$p"
done

. $IDF_PATH/export.sh
idf.py set-target esp32c6
idf.py build
```

Output: `build/network_adapter.bin`. Flash via the **back** USB-C port
(CH334 hub reaches the C6 directly); hold `SW1` (BOOT) low at reset to
enter ROM download mode, then:

```bash
esptool --chip esp32c6 -p /dev/cu.usbmodem<your-c6-port> \
  --before default-reset --after hard-reset \
  write-flash 0x0 build/network_adapter.bin
```

(In-band OTA from Linux is also possible once Wi-Fi is up; see
`drivers/net/wireless/espressif/esp_hosted/main.c` `ota_file=` module
parameter.)

See `patches/c6-slave/README.md` for the per-patch breakdown.

## Flashing

From macOS where the badge is plugged in (side USB-C reaches the P4 via
CH340; close `tio` first or esptool will fail with "port busy"):

```bash
cd /path/to/buildroot

esptool --chip esp32p4 -p /dev/cu.wchusbserial10 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 40m \
  0x2000   /path/to/why2025-linux/linux-native/build/bootloader/bootloader.bin \
  0x8000   /path/to/why2025-linux/linux-native/build/partition_table/partition-table.bin \
  0x10000  /path/to/why2025-linux/linux-native/build/linux-native.bin \
  0x90000  output/images/Image \
  0x710000 output/images/rootfs.squashfs \
  0xF10000 output/build/linux-6.18.26/arch/riscv/boot/dts/espressif/esp32p4-why2025.dtb
```

When only the kernel changed, drop the rootfs/dtb lines. When only
userspace changed, flash rootfs alone (offset 0x710000).

## Running sensorpanel

`sensorpanel` is the BME680 + BMI270 live-readout demo on the DSI
panel. It is **not** started at boot by default. Two ways to launch it:

```sh
# Manual one-shot (after login). Ctrl+C to exit.
/usr/bin/sensorpanel
```

```sh
# Enable auto-start on every boot. Recovery: rm /etc/sensorpanel.enable
touch /etc/sensorpanel.enable
```

The auto-start path is gated by `/etc/init.d/S98sensorpanel`; without
the gate file present it short-circuits at the top and does nothing.

## Running fbDOOM

The reference Buildroot config pulls `fbdoom` and a shareware WAD
(`BR2_PACKAGE_FBDOOM=y` + `BR2_PACKAGE_DOOM_WAD=y`) so they're already
in the rootfs.

After login on the badge:

```sh
# 6 MB Z-zone — biggest setting that fits the userspace mmap pool reserved
# by patch 0032 (10 MB at 0x49600000). -iwad path may differ depending on
# the Buildroot version; check /usr/share/games/doom/ first.
fbdoom -iwad /usr/share/games/doom/doom1.wad -mb 6
```

fbdoom takes over /dev/fb0, so fbcon stops drawing on the panel for
the duration of the game. The keypad maps to F1..F6 + arrow keys; see
the DTS keymap for the full set. Backspace exits.

If you hit `Z_Init: alloc failed` or `mmap failed` on startup, that's
the NOMMU pool exhaustion failure — `-mb 6` is the safe ceiling on this
build. Smaller values (e.g. `-mb 2`) work in even more constrained
configurations.

## Monitoring

```bash
tio /dev/cu.wchusbserial10        # interactive
python3 tools/bootcap.py 25       # 25 s boot capture (close tio first)
python3 tools/freezetest.py 30 90 # 30-cycle reliability test
```

See [`tools/README.md`](tools/README.md) for the full harness.

## Layout reference

| Offset    | Size   | Content              |
|-----------|--------|----------------------|
| 0x002000  | 24 KB  | ESP-IDF bootloader   |
| 0x008000  | 4 KB   | Partition table      |
| 0x009000  | 24 KB  | NVS                  |
| 0x00f000  | 4 KB   | PHY init             |
| 0x010000  | 0.5 MB | Boot shim app        |
| 0x090000  | 6.5 MB | Linux Image          |
| 0x710000  | 8 MB   | rootfs (squashfs, must be POW2) |
| 0xF10000  | 64 KB  | Device tree blob     |

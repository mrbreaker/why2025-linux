# Building why2025-linux

Three artifacts, in order:

  1. **Kernel + rootfs** via Buildroot 2025.02.15 (Linux 6.18.35 LTS).
  2. **Boot shim** via ESP-IDF v5.5.3 — loads the kernel into PSRAM and
     jumps to it.
  3. **C6 slave firmware** via ESP-IDF — Wi-Fi/BT/backlight coprocessor.

(1) needs Linux — Buildroot doesn't run on macOS. (2) and (3) build
fine natively on macOS; the reference setup builds them in the same
Linux VM as (1) for one toolchain, but that's not required.

Hitting an error that doesn't look like it belongs to you? Check
[Troubleshooting](#troubleshooting) first — several of these are host-
environment gotchas, not bugs in this repo.

## Getting the source

Clone Buildroot as `buildroot/` inside this checkout — `.gitignore`
and every path below assume this layout, and `post-image.sh` publishes
flashable images to `buildroot/output/images/` relative to it:

```bash
git clone -b 2025.02.15 https://gitlab.com/buildroot.org/buildroot
```

2025.02.x is Buildroot's current LTS, supported until ~March 2028 —
stay on its point releases rather than the quarterly 2025.05+/2026.xx
releases, which churn toolchain defaults.

Buildroot's `.config` and `CONFIG_EXTRA_FIRMWARE_DIR` only accept
absolute paths, so `configs/why2025_defconfig` and
`patches/linux/kernel.config` ship with the token `@WHY2025_LINUX@`
wherever this repo's path is needed. Substitute it from the repo root:

```bash
./setup-paths.sh
```

Idempotent — safe to re-run after `git pull`. Confirms itself:
`grep -r '@WHY2025_LINUX@' configs patches/linux/*.config` should
return nothing. By hand:
`sed -i.bak "s|@WHY2025_LINUX@|$PWD|g" configs/why2025_defconfig patches/linux/kernel.config`.

## Environment setup

  - **Linux host** for step 1 (Ubuntu 24.04 reference). On macOS, use
    a VM:
      - **OrbStack** — `orb create ubuntu rv32dev`. Shares the Mac
        filesystem into the VM at the same paths, so this checkout is
        reachable from inside the VM unchanged. Run one-shot commands
        with `orb -m rv32dev bash -c '...'`.
      - **Lima**, **Multipass**, **UTM** work the same way.

    Build Buildroot's `output/` on the VM's native disk, not the
    shared macOS mount — see
    [Troubleshooting](#too-many-open-files-building-on-a-mac-shared-vm-mount)
    if you skip this and hit `Too many open files`.

    USB passthrough to a VM is unreliable, so flash and monitor serial
    from the host OS, not the VM — the badge enumerates there directly.

  - **Host packages** (Ubuntu/Debian):
    ```bash
    sudo apt update && sudo apt install -y \
      build-essential file bc cpio rsync unzip git wget \
      libncurses-dev libssl-dev python3 perl
    ```
    A minimal/cloud image is usually missing several of these — see
    [Troubleshooting](#missing-host-packages).

  - **ESP-IDF v5.5.3** for the boot shim (step 2), per
    https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32p4/get-started/.
    Set `IDF_PATH`, then `source $IDF_PATH/export.sh` in every shell
    before running `idf.py` — it doesn't persist across shells. The
    commands below repeat it for that reason; skip it if you've
    already sourced it in the current shell. The C6 slave (step 3)
    manages its own separate ESP-IDF checkout — see that step.

  - **Flash tools**, on whichever host the badge is plugged into:
    `pip install esptool pyserial`, and `tio` for the serial console.

## 1. Kernel + rootfs

```bash
cd /path/to/why2025-linux/buildroot
cp ../configs/why2025_defconfig .config
make olddefconfig
make -j$(nproc)
```

The defconfig wires `BR2_LINUX_KERNEL_PATCH`,
`BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE`, `BR2_ROOTFS_OVERLAY`, and the
post-build/post-image scripts to this repo already. To override at the
command line instead of running `setup-paths.sh`:

```bash
make BR2_LINUX_KERNEL_PATCH=/path/to/why2025-linux/patches/linux \
     BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=/path/to/why2025-linux/patches/linux/kernel.config \
     ...
```

`patches/buildroot/post-image.sh` publishes to
`buildroot/output/images/` after every build, in-tree or via the
`O=<dir>` layout in [Troubleshooting](#too-many-open-files-building-on-a-mac-shared-vm-mount):

  - `Image` — flat kernel image (~6.5 MB)
  - `rootfs.squashfs` — read-only root (~5 MB)
  - `esp32p4-why2025.dtb` — pulled out of the kernel build tree, since
    the kernel packaging here doesn't install DTBs into `output/images/`

The rootfs overlay (`patches/buildroot/overlay/`) applies automatically.
`patches/buildroot/post-build.sh` chmods init scripts.

Faster rebuilds (prefix `O=~/br-output` too if you're using that layout):
```bash
# Just the kernel after a patch change
make linux-rebuild all -j$(nproc)

# Re-apply kernel.config after editing it
make linux-reconfigure all -j$(nproc)
```

> **Note:** the overlay rsync only *adds* files. Removing something
> from `patches/buildroot/overlay/` leaves it in `output/target/` from
> the previous build. Run `rm -rf output/target` (or `make clean`)
> after shrinking the overlay.

## 2. Boot shim

```bash
cd /path/to/why2025-linux/linux-native
. $IDF_PATH/export.sh
idf.py build
```

Output: `build/linux-native.bin` at flash offset `0x10000`.

The boot shim:
  - Maps the rootfs partition into the IDF cache window.
  - Prewalks it (coarse 64 KB stride, fine 64 B stride over the last
    64 KB) to avoid near-EOF squashfs metadata read errors — see
    `linux-native/main/main.c`.
  - Patches the DTB's `flash@deadbeef` placeholder with the real VA + size.
  - Jumps to `0x48000000`.

## 3. C6 slave firmware

A small fork of esp-hosted-ng v1.0.6, not vendored here — clone
upstream and apply our patches. This one needs its own ESP-IDF, not
the v5.5.3 from Environment setup — `esp_driver/setup.sh` clones and
patches the pinned version esp-hosted-ng actually builds against:

```bash
git clone -b release/ng-1.0.6 https://github.com/espressif/esp-hosted.git
cd esp-hosted/esp_hosted_ng/esp/esp_driver
./setup.sh

cd network_adapter
for p in /path/to/why2025-linux/patches/c6-slave/00[0-9][0-9]-*.patch; do
    patch -p1 -i "$p"
done

. ../esp-idf/export.sh
rm -f sdkconfig          # else set-target keeps a stale config
idf.py set-target esp32c6
idf.py build
```

Patch 0005 pins the transport to SPI (esp-hosted-ng defaults the C6 to
SDIO, which won't talk to the P4's SPI host — no `wlan0`). Confirm from
the C6's boot banner on its USB console that it reads `Transport used
:: SPI only`, not `SDIO only`.

Flash via the **bottom** USB-C port (the C6's native USB — see
`HARDWARE.md`; find the port with `ls /dev/cu.*`). Easiest:

```bash
idf.py -p /dev/cu.usbmodem<your-c6-port> flash
```

Or with esptool directly — all four images at their offsets from
`build/flasher_args.json`:

```bash
esptool --chip esp32c6 -p /dev/cu.usbmodem<your-c6-port> \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xd000  build/ota_data_initial.bin \
  0x10000 build/network_adapter.bin
```

> **Warning:** do NOT flash `network_adapter.bin` alone at `0x0` (an
> older revision of this doc said to) — that's the app image, and at
> `0x0` it overwrites the C6's bootloader, leaving the C6 unbootable
> until reflashed correctly.

In-band OTA from Linux also works once Wi-Fi is up — see
`drivers/net/wireless/espressif/esp_hosted/main.c`'s `ota_file=`
module parameter.

See `patches/c6-slave/README.md` for the per-patch breakdown.

## Flashing

Badge plugged in (side USB-C reaches the P4 via CH340; close `tio`
first or esptool fails with "port busy"; find your port with
`ls /dev/cu.*`):

```bash
cd /path/to/why2025-linux/buildroot

esptool --chip esp32p4 -p /dev/cu.wchusbserial<your-p4-port> -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 40m \
  0x2000   ../linux-native/build/bootloader/bootloader.bin \
  0x8000   ../linux-native/build/partition_table/partition-table.bin \
  0x10000  ../linux-native/build/linux-native.bin \
  0x90000  output/images/Image \
  0x710000 output/images/rootfs.squashfs \
  0xF10000 output/images/esp32p4-why2025.dtb
```

Kernel-only change: drop the rootfs/dtb lines. Userspace-only change:
flash rootfs alone (offset 0x710000).

## Running sensorpanel

`sensorpanel` renders live BME680 + BMI270 readings to the DSI panel.
Not started at boot by default:

```sh
# Manual one-shot. Ctrl+C to exit.
/usr/bin/sensorpanel
```

```sh
# Auto-start on every boot. Recovery: rm /etc/sensorpanel.enable
touch /etc/sensorpanel.enable
```

Gated by `/etc/init.d/S98sensorpanel`, which no-ops without the gate file.

## Running fbDOOM

The saved config enables `BR2_PACKAGE_FBDOOM=y` + `BR2_PACKAGE_DOOM_WAD=y`.

> **Warning:** `fbdoom`/`doom-wad` aren't upstream Buildroot packages —
> local additions in the reference build tree, not yet vendored here.
> A fresh Buildroot clone drops both options silently at
> `olddefconfig`. Copy the package directories into the new clone's
> `package/` (and re-add the `source "package/fbdoom/Config.in"` /
> `"package/doom-wad/Config.in"` lines to `package/Config.in`) first.

After login:

```sh
# 6 MB Z-zone — the largest that fits the nommu-userspace-pool reserved
# by patch 0010. -iwad path varies; check /usr/share/games/doom/ first.
fbdoom -iwad /usr/share/games/doom/doom1.wad -mb 6
```

Takes over `/dev/fb0` — fbcon stops drawing until it exits. Keypad:
F1..F6 + arrows (see the DTS keymap for the full set), Backspace exits.

`Z_Init: alloc failed` or `mmap failed` at startup means NOMMU pool
exhaustion — `-mb 6` is the ceiling here; try `-mb 2` on more
constrained builds.

## Monitoring

```bash
tio /dev/cu.wchusbserial<your-p4-port>  # interactive
python3 tools/bootcap.py 25             # 25 s boot capture (close tio first)
python3 tools/freezetest.py 30 90       # 30-cycle reliability test
```

The `tools/*.py` scripts hardcode `/dev/cu.wchusbserial10` internally —
edit the device path in the script if yours differs.

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

## Troubleshooting

### Missing host packages

A minimal/cloud Ubuntu image (a fresh `orb create ubuntu` included) is
usually missing packages Buildroot's dependency check requires, or
that the kernel build needs later:

```bash
sudo apt update && sudo apt install -y \
  build-essential file bc cpio rsync unzip git wget \
  libncurses-dev libssl-dev python3 perl
```

`file` is usually the first one missing — fails `make` at the
`dependencies` target before any real build starts (Buildroot's
required-package list:
https://buildroot.org/downloads/manual/manual.html#requirement).
`libssl-dev` shows up later: without it the kernel build fails at
`certs/extract-cert.c: openssl/bio.h: No such file or directory` — its
module-signing cert-extraction tool needs OpenSSL headers regardless
of this project's kernel config.

### `/usr/bin/install` is uutils-coreutils, not GNU coreutils

Ubuntu releases past 24.04 LTS default some tools to the Rust
`uutils-coreutils` reimplementation, and Buildroot's dependency check
rejects `install` from it (known gaps:
https://github.com/uutils/coreutils/issues/12166). The distro ships
the GNU binary alongside it for this transition:

```bash
sudo update-alternatives --install /usr/bin/install install /usr/bin/gnuinstall 100
sudo update-alternatives --set install /usr/bin/gnuinstall
```

The `--set` is required — `--install` alone doesn't take effect if
`install` is already alternatives-managed. Verify with
`install --version` (should say "GNU coreutils"). Hitting this means
your host isn't actually 24.04 LTS — watch for the same issue on other
coreutils-backed tools.

### "Too many open files" building on a Mac-shared VM mount

Building Buildroot's `output/` on the shared macOS↔VM mount is several
times slower than native VM disk, and on OrbStack a `make -j$(nproc)`
kernel build can fail outright there: `fixdep: error opening file: ...
Too many open files`, `gcc: ... Too many open files`.

Not a guest limit — `ulimit -n` and `/proc/sys/fs/file-max` inside the
VM can look completely unconstrained and it still happens, because the
ceiling is on the macOS **host**: OrbStack's virtiofs bridge (Apple's
VirtioFS) holds one file descriptor open per accessed inode until the
guest sends `FUSE_FORGET`, which lags under heavy concurrency, and that
host process is bound by macOS's `kern.maxfilesperproc` (check with
`sysctl kern.maxfilesperproc` on macOS, not the VM). A parallel kernel
build opens enough headers at once to blow past it.

Two fixes, cheapest first:

1. `make -j8` — gives `FUSE_FORGET` time to keep up. No setup, slower.
2. Move `output/` off the shared mount, via Buildroot's `O=<dir>`:
   ```bash
   mkdir -p ~/br-output
   cp /path/to/why2025-linux/configs/why2025_defconfig ~/br-output/.config
   cd /path/to/buildroot
   make O=~/br-output olddefconfig
   make O=~/br-output -j$(nproc)
   ```
   The Buildroot checkout can stay where it is. `~/br-output` sits on
   the VM's native disk, taking `build/`, `host/`, `staging/`,
   `target/`, `images/` off virtiofs with it.
   `BR2_ROOTFS_POST_IMAGE_SCRIPT` (`patches/buildroot/post-image.sh`)
   still publishes to `<repo>/buildroot/output/images/` regardless, so
   the build/flashing commands above don't change.

   Raising `kern.maxfiles`/`kern.maxfilesperproc` on macOS and
   restarting OrbStack works too, but it's host-wide and doesn't
   survive a reboot.

   Pass `O=~/br-output` on every subsequent command too — `menuconfig`,
   `savedefconfig`, `clean`, the rebuild targets, all of it. Buildroot's
   `Makefile` only honors `O` from the command line
   (`ifneq ("$(origin O)", "command line")`); `export O=~/br-output`
   is silently ignored and falls back to the in-tree `output/` — empty,
   no `.config` — so it looks like the build vanished. Alias it instead:
   ```bash
   alias mbr='make O=~/br-output'
   ```

   Lima/Multipass/UTM have the same tradeoff with their own
   shared-folder mechanisms, though the exact failure differs.

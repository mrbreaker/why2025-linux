# Buildroot rootfs overlay + userspace utilities

This directory carries everything Buildroot needs beyond the kernel
patches: rootfs overlay files and small C utilities that get built into
FLAT binaries and dropped into `/usr/bin/`.

## Layout

```
overlay/        Files copied verbatim into the target rootfs (via
                BR2_ROOTFS_OVERLAY). Includes /etc/profile,
                /etc/inittab, /etc/init.d/*, /lib/firmware/*, and the
                wifi-connect shell script.

overlay-src/    C sources for the FLAT userspace binaries. Build with
                `make` here BEFORE the buildroot rootfs is generated;
                outputs land in overlay/usr/bin/ and get rsync'd into
                the target.

buildroot-tree/ Patches applied to the Buildroot checkout itself
                (`patch -p1 -d buildroot`) right after cloning — see
                BUILDING.md "Getting the source". Currently: drop
                wpa_supplicant's `depends on BR2_USE_MMU` so olddefconfig
                stops silently discarding it.

global-patches/ BR2_GLOBAL_PATCH_DIR tree — per-package source patches
                Buildroot applies on top of its own. Currently:
                wpa_supplicant os_exec() fork()→vfork() so it links
                against the fork-less NOMMU uClibc-ng.

post-build.sh   Buildroot post-build hook (e.g. chmod init scripts).

post-image.sh   Buildroot post-image hook (BR2_ROOTFS_POST_IMAGE_SCRIPT).
                Publishes the final Image/rootfs.squashfs/DTB to
                <repo>/buildroot/output/images/ regardless of whether
                the build was in-tree or via `make O=<dir>` — see
                BUILDING.md's "Building on macOS" section for why the
                latter matters on OrbStack.
```

## The userspace utilities

| Binary | Source | What it does |
|--------|--------|--------------|
| `sensorpanel` | `overlay-src/sensorpanel.c` | RGB565 dashboard rendering BME680 + BMI270 readings to /dev/fb0 at ~3 Hz. Uses an mmap-anon shadow + pwrite per frame (drm_fbdev_dma's defio path doesn't expose its shadow to userspace mmap on this NOMMU build). |
| `ble_scan` | `overlay-src/ble_scan.c` | Custom BLE scanner over `HCI_CHANNEL_USER`. Sends Reset, sets Event Mask + LE Event Mask (HCI_Reset wipes them back to spec defaults that exclude LE Meta — must re-set after Reset), enables LE Scan, prints adverts. |
| `hci_up` | `overlay-src/hci_up.c` | Helper to `hciconfig hci0 up` without pulling in BlueZ (NOMMU FLAT can't link BlueZ's stack). |
| `wifi-connect` | `overlay/usr/bin/wifi-connect` | NOMMU-aware shell script: bring up wlan0 against the C6 over esp-hosted-NG. wpa_supplicant + udhcpc, fork-budgeted to fit the buddy heap. |

## Building the FLAT binaries

```bash
cd patches/buildroot/overlay-src
make
```

This requires the buildroot toolchain to be available at
`$HOME/buildroot/output/host/bin/riscv32-buildroot-linux-uclibc-gcc`
(the `Makefile`'s default; override with `make BUILDROOT=/path/to/buildroot`
or `make T=/abs/path/to/riscv32-buildroot-linux-uclibc` if your tree
lives elsewhere).
Outputs are placed directly into `../overlay/usr/bin/` and rsync'd into
the rootfs by buildroot.

Build flags learned the hard way (see `Makefile`):

  - `-fPIC` — generates GOT-relative addressing; without it, elf2flt
    silently drops `R_RISCV_32` relocs and the binary SIGSEGVs on load.
  - `-Wl,-elf2flt="-r"` — ask elf2flt to emit a RAM-loaded BFLT with a
    relocation table.
  - **Avoid uClibc stdio.** `printf`/`fprintf` etc. reference pthread
    and stdio_streams globals that lose relocations through elf2flt.
    Use raw `write(2)` / `read(2)` and a hand-rolled `say_int()` for
    integer formatting (see `sensorpanel.c` for the pattern).

Verify the output is a correctly-relocated FLAT:

```bash
$T-flthdr overlay/usr/bin/sensorpanel
# Should report "ram gotpic" + Reloc Count > 0
```

## NOMMU sizing

Every fork+exec rounds the binary's text+data+bss+stack up to the next
power-of-two and allocates as one contiguous block from the buddy heap.
On this 32 MB PSRAM build:

  - busybox is trimmed to 459 KB → order-7 (512 KB) block.
  - wpa_supplicant is ~999 KB all-in (text+data+bss+stack per flthdr,
    with BR2_PACKAGE_WPA_SUPPLICANT_AP_SUPPORT off — AP+P2P alone are
    ~760 KB) → order-8 (1 MB) block, with only ~48 KB of headroom.
    Check `flthdr` after any wpa_supplicant or toolchain change.
  - sensorpanel ~62 KB → order-7 fork allocation.

If you add a new utility, audit its size and round up. Anything over
1 MB likely won't load on a fragmented heap. See the upstream
`feedback_nommu_flat_sizing` discussion in the project's design notes.

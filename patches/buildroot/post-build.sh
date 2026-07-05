#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Buildroot ROOTFS_POST_BUILD_SCRIPT for the WHY2025 badge native-Linux port.
#
# Buildroot generates an inittab that respawns getty on /dev/console.
# Our kernel cmdline lists console=ttyS0 first then console=tty1, so the
# last entry wins and /dev/console = /dev/tty1 — which means Buildroot's
# default getty lives on the framebuffer console (the badge's screen),
# not on the serial UART.
#
# We want both: a screen-side shell (for when the KeebDeck CH32V203
# keyboard support lands in Phase 2) AND a serial-side shell on ttyS0
# (so `tio` retains an interactive prompt today). Append a respawn for
# ttyS0 to /etc/inittab; the existing /dev/console respawn (i.e. tty1)
# stays in place.
#
# Idempotent: re-running the script doesn't double the inittab entry.
set -eu

TARGET_DIR="${1:-${TARGET_DIR:-}}"
if [ -z "${TARGET_DIR}" ]; then
    echo "post-build.sh: TARGET_DIR not set" >&2
    exit 1
fi

INITTAB="${TARGET_DIR}/etc/inittab"
LINE="ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100"

if [ ! -f "${INITTAB}" ]; then
    echo "post-build.sh: ${INITTAB} not found" >&2
    exit 1
fi

if ! grep -qF "${LINE}" "${INITTAB}"; then
    printf '\n# WHY2025: serial getty on ttyS0 (in addition to /dev/console = tty1)\n%s\n' \
        "${LINE}" >> "${INITTAB}"
fi

# WHY2025: raise dropbear's FLAT stack 4 KB -> 16 KB (kex/crypt stack
# headroom; the elf2flt default is 4 KB). In-memory image becomes
# 514,288 B (with the client programs) — still inside the 512 KB
# order-7 bucket with ~10 KB spare, see
# patches/buildroot/dropbear-localoptions.h. flthdr comes from
# HOST_DIR/bin, which Buildroot puts first on PATH for this script.
# Idempotent: -s sets an absolute value.
if [ -f "${TARGET_DIR}/usr/sbin/dropbear" ]; then
    riscv32-buildroot-linux-uclibc-flthdr -s 16384 "${TARGET_DIR}/usr/sbin/dropbear"
fi

# WHY2025: locally-built FLAT tools leave elf2flt debug sidecars (*.gdb,
# ~200 KB host-side ELFs each) next to the binaries in the overlay; the
# overlay rsync copies them into the target. Strip them from the image.
rm -f "${TARGET_DIR}"/usr/bin/*.gdb

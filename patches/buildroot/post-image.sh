#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Buildroot ROOTFS_POST_IMAGE_SCRIPT for the WHY2025 badge native-Linux port.
#
# Building the kernel with `make O=<dir>` against a directory on the
# Linux VM's own native disk (instead of the Buildroot checkout's
# default in-tree `output/`, which on an OrbStack-on-macOS setup is
# typically the virtiofs-shared macOS mount) sidesteps a host-side
# file-descriptor exhaustion bug: virtiofs's macOS-host process holds
# one fd per accessed inode until the guest sends FUSE_FORGET, and a
# high-parallelism kernel build (`make -j$(nproc)`) can open enough
# concurrent inodes to blow past the host's own kern.maxfilesperproc
# ceiling, surfacing as spurious "Too many open files" errors inside
# the guest even though every guest-side ulimit/sysctl looks fine. See
# BUILDING.md's "Building on macOS" section.
#
# The tradeoff: with O=<dir> pointed off the shared mount, the final
# images (and the DTB, which this project's flash command reads
# straight out of the kernel build tree rather than BINARIES_DIR) land
# somewhere macOS can't see directly for flashing. This script runs
# after every build (in-tree or out-of-tree) and publishes them to the
# fixed, gitignored path BUILDING.md's flash commands expect --
# <this repo>/buildroot/output/images/ -- so the documented flashing
# steps work unchanged regardless of which build layout produced them.
#
# Buildroot always invokes this script by its absolute
# BR2_ROOTFS_POST_IMAGE_SCRIPT path, so $0 is reliably this repo's own
# location -- self-locate from it rather than relying on the
# @WHY2025_LINUX@ token, which setup-paths.sh does NOT substitute in
# this file (it only patches configs/why2025_defconfig and
# patches/linux/kernel.config).
set -eu

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "${SELF_DIR}/../.." && pwd)"

BINARIES_DIR="${1:?post-image.sh: BINARIES_DIR arg missing}"
BASE_DIR="$(dirname "${BINARIES_DIR}")"
DEST="${REPO_DIR}/buildroot/output/images"

mkdir -p "${DEST}"

# In-tree builds already have BINARIES_DIR == DEST; skip the no-op copy.
if [ "$(cd "${BINARIES_DIR}" && pwd)" != "$(cd "${DEST}" && pwd)" ]; then
    cp -a "${BINARIES_DIR}"/. "${DEST}"/
fi

# The DTB isn't installed into BINARIES_DIR by this project's kernel
# packaging; find it under build/ regardless of the kernel source
# directory's name (varies with the fetch method -- linux-v6.18.35 for
# a git checkout, linux-custom for a tarball, ...) and publish it
# alongside the other images under a stable name.
dtb=$(find "${BASE_DIR}/build" -maxdepth 8 \
      -path '*/arch/riscv/boot/dts/espressif/esp32p4-why2025.dtb' \
      -print -quit 2>/dev/null || true)
if [ -n "${dtb}" ]; then
    cp -a "${dtb}" "${DEST}/esp32p4-why2025.dtb"
else
    echo "post-image.sh: warning: esp32p4-why2025.dtb not found under ${BASE_DIR}/build" >&2
fi

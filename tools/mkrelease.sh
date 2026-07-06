#!/bin/sh
# SPDX-License-Identifier: MIT
#
# mkrelease.sh — build the two ready-to-flash release images.
#
# Merges every flash artifact for a chip into one 0x0-based binary
# (esptool merge-bin pads the gaps), so a user needs exactly one
# write-flash per chip:
#
#   esptool --chip esp32p4 -p /dev/<p4-port> -b 460800 write-flash 0x0 esp32p4.bin
#   esptool --chip esp32c6 -p /dev/<c6-port> -b 460800 write-flash 0x0 esp32c6.bin
#
# (Two ports: the side USB-C is the P4 via CH340, the bottom USB-C is
# the C6's native USB. See HARDWARE.md.)
#
# Usage:
#   tools/mkrelease.sh <images-dir> <shim-build-dir> <c6-build-dir> <out-dir>
#
#   images-dir      buildroot output/images (Image, rootfs.squashfs, dtb)
#   shim-build-dir  linux-native/build (bootloader, partition-table, shim)
#   c6-build-dir    esp-hosted network_adapter/build, or "-" to skip the C6
#   out-dir         where esp32p4.bin / esp32c6.bin / SHA256SUMS land
#
# P4 offsets must match linux-native/partitions.csv and BUILDING.md.
set -eu

IMAGES="${1:?images dir}"
SHIM="${2:?shim build dir}"
C6="${3:?c6 build dir or -}"
OUT="${4:?output dir}"

mkdir -p "$OUT"

esptool --chip esp32p4 merge-bin -o "$OUT/esp32p4.bin" \
    --flash-mode dio --flash-size 16MB --flash-freq 40m \
    0x2000   "$SHIM/bootloader/bootloader.bin" \
    0x8000   "$SHIM/partition_table/partition-table.bin" \
    0x10000  "$SHIM/linux-native.bin" \
    0x90000  "$IMAGES/Image" \
    0x710000 "$IMAGES/rootfs.squashfs" \
    0xF10000 "$IMAGES/esp32p4-why2025.dtb"

if [ "$C6" != "-" ]; then
    # Offsets come from the C6 build itself; flasher_args.json is the
    # authority (bootloader/partition-table/ota_data/app).
    python3 - "$C6" "$OUT" <<'EOF'
import json, subprocess, sys
c6, out = sys.argv[1], sys.argv[2]
args = json.load(open(c6 + "/flasher_args.json"))
cmd = ["esptool", "--chip", "esp32c6", "merge-bin", "-o", out + "/esp32c6.bin",
       "--flash-mode", args["flash_settings"]["flash_mode"],
       "--flash-size", args["flash_settings"]["flash_size"],
       "--flash-freq", args["flash_settings"]["flash_freq"]]
for off, path in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    cmd += [off, c6 + "/" + path]
sys.exit(subprocess.call(cmd))
EOF
fi

cd "$OUT"
shasum -a 256 esp32p4.bin ${C6:+$([ "$C6" != - ] && echo esp32c6.bin)} > SHA256SUMS 2>/dev/null || \
    sha256sum esp32p4.bin $([ "$C6" != - ] && echo esp32c6.bin) > SHA256SUMS
cat SHA256SUMS

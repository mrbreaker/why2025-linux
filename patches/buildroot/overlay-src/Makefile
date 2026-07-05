# Tiny FLAT binaries dropped into the rootfs overlay alongside Buildroot output.
# Buildroot doesn't build these — they're cross-compiled here and copied
# verbatim into output/target via the BR2_ROOTFS_OVERLAY mechanism.
#
# Usage (from inside OrbStack):
#   make            build all binaries
#   make clean
#
# Required flags (learned the hard way):
# - -fPIC          generates GOT-relative addressing; without it, elf2flt
#                  silently drops R_RISCV_32 relocations -> SIGSEGV at load
# - -Wl,-elf2flt="-r"   produce a RAM-loaded BFLT with relocation table
# - avoid stdio    uClibc's printf/fprintf reference pthread/stdio_streams
#                  globals that lose relocations through elf2flt; use raw
#                  write(2) plus the hand-rolled say_int() helper
#
# Verify the output is correct with:
#   $T-flthdr <file>     should report "ram gotpic" + Reloc Count > 0

# Toolchain prefix. Override via env or arg, e.g.:
#   make BUILDROOT=/path/to/buildroot
#   make T=/abs/path/to/riscv32-buildroot-linux-uclibc
BUILDROOT ?= $(HOME)/buildroot
T         ?= $(BUILDROOT)/output/host/bin/riscv32-buildroot-linux-uclibc
CC         = $(T)-gcc
CFLAGS  = -fPIC -static -Os -Wall -static-libgcc \
          -ffunction-sections -fdata-sections
LDFLAGS = -Wl,-elf2flt="-r" -Wl,--gc-sections

OUT = ../overlay/usr/bin
BINS = $(OUT)/ble_adv $(OUT)/ble_scan $(OUT)/hci_up $(OUT)/sensorpanel $(OUT)/sleepd

all: $(BINS)

$(OUT)/%: %.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(BINS) $(OUT)/*.gdb

.PHONY: all clean

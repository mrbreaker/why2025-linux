# C6 slave patches against upstream esp-hosted-ng

Patch series against the slave application from
[`espressif/esp-hosted`](https://github.com/espressif/esp-hosted),
tag `release/ng-1.0.6`. The slave lives in
`esp_hosted_ng/esp/esp_driver/network_adapter/` upstream; we apply
patches relative to that directory (so `a/main/foo.c` →
`network_adapter/main/foo.c`).

## Active series

| #    | Patch | What it does |
|------|-------|--------------|
| 0001 | `spi-slave-c6-gpio-matrix-overrides.patch` | SPI pin overrides for the WHY2025 badge: MOSI=18, MISO=20, SCK=19, CS=21 (mapped via the C6 GPIO matrix to align with the badge's SDIO trace pins). Replaces the upstream defaults in the C6 stanza of `spi_slave_api.c`. |
| 0002 | `cmd-set-backlight-slot-32.patch` | Adds `CMD_SET_BACKLIGHT = 32` to `include/cmd.h`, `struct cmd_set_backlight { command_header header; u8 brightness; u8 pad[3]; }` to `include/adapter.h`, and the `process_set_backlight()` handler to `cmd.c`. The handler does `ledc_set_duty + ledc_update_duty` on `LEDC_CHANNEL_0` (display backlight). |
| 0003 | `app-main-why2025-backlight-init.patch` | Adds `why2025_backlight_init()` (sets up LEDC timer 0 + channels for display + keyboard backlight, `BL_DISPLAY_INITIAL_DUTY=76`), wires the `CMD_SET_BACKLIGHT` dispatch into `process_priv_cmd()`, and calls the init at boot. Also includes a small SLC-bootup-retrigger loop that's only relevant for the SDIO transport and is harmless on SPI builds. |
| 0005 | `c6-spi-transport-and-handshake-gpios.patch` | Pins the C6 transport to **SPI** in `sdkconfig.defaults.esp32c6`. esp-hosted-ng defaults the C6 to SDIO (`ESP_HOST_INTERFACE` = `ESP_SDIO_HOST_INTERFACE if SOC_SDIO_SLAVE_SUPPORTED`), so a clean `idf.py set-target esp32c6` build comes up on the wrong bus and never handshakes with the P4's SPI host — no `wlan0`. Also sets the handshake/data-ready GPIOs to the badge's wiring (`HANDSHAKE=22`, `DATA_READY=23`; the SPI Kconfig defaults are 3/4). The four data pins are already set by patch 0001. |

## Disabled / parked

| Patch | Note |
|-------|------|
| `0004-sdio-slave-c6-v02-quirks.patch.disabled` | C6 v0.2 SDIO slave silicon-and-firmware quirks: pkt_len strobe gates DAT drive; conf_w5 must be individual size not cumulative; spurious slc0_rx_eof handling. Required for the SDIO transport (now historical — the badge's runtime path is SPI). Kept here for completeness and in case the SDIO path is revisited. |

## Applying and building

The C6 slave needs its own ESP-IDF, not your system `$IDF_PATH` —
esp-hosted-ng pins a specific v5.5.1 commit and ships its own Wi-Fi
library set (`esp_driver/lib/`) with MLME-offload symbols
(`esp_wifi_get_eb_data`, `ieee80211_add_node`, ...) that stock v5.5.3
doesn't export for esp32c6, failing at link time otherwise.
`esp_driver/setup.sh` clones the pinned ESP-IDF into
`esp_driver/esp-idf/`, applies its ROM linker-script patch, and
replaces `esp-idf/components/esp_wifi/lib/` with esp-hosted's set.

```bash
git clone -b release/ng-1.0.6 https://github.com/espressif/esp-hosted.git
cd esp-hosted/esp_hosted_ng/esp/esp_driver
./setup.sh

cd network_adapter
for p in /path/to/why2025-linux/patches/c6-slave/00[0-9][0-9]-*.patch; do
    patch -p1 -i "$p"
done
# (0004 is .disabled by default; only apply if you actually need SDIO)

. ../esp-idf/export.sh
rm -f sdkconfig          # else set-target keeps a stale (SDIO) config
idf.py set-target esp32c6
idf.py build
```

`set-target` reads `sdkconfig.defaults.esp32c6` (SPI transport pinned by
patch 0005) only when generating a fresh `sdkconfig` — hence the `rm`.
Confirm the build is SPI, not SDIO, from the C6 boot banner over its
USB console: it must read `Transport used :: SPI only`. `SDIO only`
means the config didn't take and the P4 host will never see `wlan0`.

Flash via the **bottom** USB-C port (the C6's native USB — see
`HARDWARE.md`):

```bash
idf.py -p /dev/cu.usbmodem<your-c6-port> flash
```

Or with esptool, all four images at their `build/flasher_args.json`
offsets:

```bash
esptool --chip esp32c6 -p /dev/cu.usbmodem<your-c6-port> \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 4MB --flash-freq 80m \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0xd000  build/ota_data_initial.bin \
  0x10000 build/network_adapter.bin
```

> **Warning:** do NOT flash `network_adapter.bin` alone at `0x0` — at
> `0x0` the app image overwrites the C6's bootloader, leaving the C6
> unbootable until reflashed correctly.

(In-band OTA from Linux is also possible once Wi-Fi is up; see
`drivers/net/wireless/espressif/esp_hosted/main.c` `ota_file=` module
parameter.)

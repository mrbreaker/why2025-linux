# tools/

Pyserial test harnesses run from the macOS host. The badge appears as
`/dev/cu.wchusbserial10` (CH340 on the side USB-C). All scripts pulse
RTS to hard-reset the badge before each run, so they require the
`tio` console to be **closed** beforehand (it exclusive-locks the port).

## Reliability harnesses

| Tool | Purpose |
|------|---------|
| `freezetest.py [N=20] [boot_window_s=30] [outdir=/tmp/freezetest]` | Cold-boot reliability. Runs N RTS-reset cycles, classifies each as success/freeze/freeze_alive, dumps boot+dmesg+/proc artifacts on success and last-printk-line on freeze. The harness used to validate the 95–100 % target. |
| `heartbeat_test.py [max_min=30] [output_dir=/tmp/heartbeat]` | Light-load uptime detector. Boots, logs in, runs a builtin-only heartbeat loop, reports any ≥90 s gap. Used to confirm the badge is stable when idle. |
| `loadtest.py [max_min=30] [output_dir=/tmp/loadtest]` | Heavy-load uptime: boot → detach fbcon → launch sensorpanel → run instrumented heartbeat that emits `/proc/buddyinfo` and key `/proc/meminfo` fields each cycle. Reproduces the runtime kernel wedge. |
| `greptest.py [max_min=30] [outdir=/tmp/greptest]` | Same as loadtest but workload is `(while :; do grep -rl DOESNOTEXIST / >/dev/null; sleep 1; done)`. Reproduces the runtime kernel wedge faster than sensorpanel. |
| `grepbisect.py [outdir=/tmp/grepbisect]` | Bisect: which subtree, when grepped continuously, triggers the wedge. Edit the `TARGETS` list to scope further. |
| `wedge_verify.py --port PORT [--workload W] [--max-min N] [--isolated]` | Host-sleep-immune wedge reproducer / fix-verifier. Boots, marker-syncs login, launches a workload, and classifies a wedge on the badge ROM's `HP_SYS_HP_WDT_RESET` banner (not host wall-clock — a laptop sleep can't fake a hardware watchdog reset). Default workload is the pure builtin loop `(while :; do :; done) &`; `--isolated` runs it with no other activity (does not wedge — see `docs/RUNTIME-WEDGE.md`). Reports the ROM `Saved PC`. This is the harness that reframed the wedge in July 2026. |

## One-shot diagnostics

| Tool | Purpose |
|------|---------|
| `bootcap.py [secs=14]` | RTS-reset and capture serial output for N seconds. Prints to stdout. |
| `sd_test.py` | SD-card sanity: dd matrix, VFAT mount, throughput. Best reference for the canonical `read+pump+write` driving pattern. |
| `wifi_test.py` | `wifi-connect "Radio Grandma" <psk>`, ping 1.1.1.1. |
| `bt_scan.py` | BLE scan via `/usr/bin/ble_scan`. |
| `iio_read.py` | Quick BME680/BMI270 sysfs sanity. |

## Pattern essentials

```python
import serial, time, sys

s = serial.Serial('/dev/cu.wchusbserial10', 115200, timeout=0.3)
s.dtr = False; s.rts = False; time.sleep(0.1)
s.rts = True; time.sleep(0.2); s.rts = False    # RTS-driven hard reset

def pump(secs):
    end = time.time() + secs
    while time.time() < end:
        c = s.read(8192)
        if c: out.extend(c)

pump(22)                         # wait for boot + login banner
s.write(b'root\n'); pump(3)
s.write(b'dmesg -n 1\n'); pump(2)  # critical — printk garbles typed cmds
s.write(b'<your command>\n'); pump(<budget>)
```

`tio` profile (`~/.config/tio/config`):

```
[badge]
device = /dev/cu.wchusbserial10
baudrate = 115200
log = enable
log-file = /tmp/tio.log
```

## Recovering a wedged badge

If `tools/loadtest.py` triggers the runtime wedge (kernel scheduler
death, no UART response), an RTS-only reset sometimes isn't enough. Do
a deep esptool reset:

```bash
esptool --chip esp32p4 -p /dev/cu.wchusbserial10 \
  --before default-reset --after hard-reset chip-id
```

Then re-run the harness. This also re-toggles the C6's reset line via
the boot shim.

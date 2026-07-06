# Known issues

Open issues only. Resolved ones are archived in one line each at the
bottom; the full investigation history lives in
[`RUNTIME-WEDGE.md`](RUNTIME-WEDGE.md) and the git log.

## Residual boot freeze at `mmcblk0: p1` (~3.7 s)

The one remaining boot-freeze class: console output stops right after
the SD partition-scan line; the MWDT resets the badge ≤30 s later, so
it costs one automatic retry, never a brick. Measured first-try rates:
29/30 (2026-07-05 warm campaign, card state unrecorded) vs 23/30 (same
day, SD card confirmed inserted, all 7 failures in or near this class)
— card presence is the leading hypothesis; a card-out 30-cycle
`tools/freezetest.py` campaign would settle it. Mechanism unknown.

Related gap: the watchdog arms at `subsys_initcall` (~0.3 s, patch
0034). A freeze before that is still unrecoverable without a power
cycle; arming the MWDT in the boot shim would close it.

## Cold boots are less reliable than warm resets

`freezetest.py` RTS-resets only the P4; a cable plug-in cold-boots both
chips, and that path measures worse (post-0034/0035 hand campaign:
~7/9 first-try, every failure watchdog-recovered). A proper cold-boot
campaign needs physical power cycles — a hand campaign or a smart-plug
harness; RTS cannot produce them.

## esp-hosted command channel can starve a queued command

`esp_cmd_work()` (patch 0008) bails with "Busy in another cmd
execution" without rescheduling itself (the upstream code comments
admit this). A command queued while another is in flight is only sent
if an unrelated caller kicks the workqueue before its timeout (500 ms
for the WHY2025-added commands, 5 s for cfg80211) — otherwise it
silently times out unsent. Likely why the WHY2025-added commands'
timeouts fire "often". A proper fix (rescheduling from
`process_cmd_resp()` and the timeout paths) changes the command
channel's locking and needs a boot-cycle load test — deferred.

## Confirmed-latent esp-hosted lifecycle bugs (hardening TODO)

A 2026-07-05 audit of patch 0008 confirmed real bugs that are all gated
on conditions absent in normal operation (teardown, a second slave
bootup, GFP_ATOMIC alloc failure, `write_packet` failure with the
datapath closed). Documented rather than shipped because they can't be
exercised without fault injection and the command path is
boot-race-sensitive:

- `esp_cmd.c` `esp_cmd_work` send-failure path recycles a cmd node a
  waiter still owns → double `list_add` on `cmd_free_queue`. Fix: set
  `cmd_resp` and `wake_up_interruptible` with `cur_cmd` still set; let
  the single waiter release.
- `esp_cmd.c` `reset_cmd_node` never clears `in_cmd_queue` → a reused
  pool slot can `list_del` a node no longer queued. Fix: clear the flag
  after the `list_del`.
- `esp_bt.c` `esp_bt_send_frame` double-frees on `hdev->send` failure
  (the HCI core frees on <0 return).
- `esp_bt.c` `hci_register_dev` failure leaves `adapter->hcidev`
  dangling; `esp_remove_card` deinits BT before flushing the RX
  workqueue (teardown UAF).

## CLIC interrupt-delivery erratum — avoided, not fixed

Silicon-level: an interrupt arriving cycle-coincident with a
privilege-dropping `mret` latches the core into never accepting another
interrupt (root-caused at register level via the patch-0025 tracer;
every software recovery attempt failed). Avoided since patch 0031 —
userspace runs at physical M-mode, so no `mret` ever drops privilege;
churn reproducers that wedged in 25–90 s run clean. The MWDT (patch
0019) stays armed as the backstop: if the latch ever resurfaces,
recovery is a ≤30 s auto-reset. Trade-off of M-mode userspace: user
code can touch CSRs/MMIO (NOMMU never had memory isolation anyway).
Full forensic trail: [`RUNTIME-WEDGE.md`](RUNTIME-WEDGE.md).

## Minor / cosmetic

- BME680 pressure reads ~227 kPa instead of ~100 kPa (BME688/690
  calibration-coefficient difference) — cosmetic.
- Three `bmi270-init-data.fw ... failed with error -2` lines on every
  boot: a probe-before-rootfs race; patch 0009 retries after the
  squashfs mounts and the successful load is silent. Harmless.
- `ssh`/`dbclient` in the first ~40 s after boot pauses at "Waiting for
  kernel randomness to be initialised" until the crng seeds.
- The panel needs a 16-px vertical shift workaround
  (`ESP32P4_PANEL_VSHIFT` in patch 0003) — suspected vsync-line
  miscount, revisit if it ever drifts.

## Resolved (details in git history / RUNTIME-WEDGE.md)

- **`wlan0 up` hang** — IPv6 `rs_timer` timer-wheel corruption, not the
  CLIC erratum; shipped fix is `ipv6.disable=1` on the cmdline
  (2026-07-05, soak-verified).
- **Cold boots wedging at the ~21 s Bluetooth HCI burst** — HCI
  registration is opt-in at runtime since patch 0035 (the HCI tools set
  the `bt_enable` knob themselves); verified by a cold-boot hand
  campaign.
- **Freezes before the watchdog armed were hard-dead** — patch 0034
  arms the MWDT at `subsys_initcall`, ahead of every device probe.
- **Boot freezes in the display/backlight bring-up window** — the CLIC
  erratum during boot's busiest interrupt window; first-try success
  went ~60% → ~73% (patch 0030, BT-init deferral) → ~97% (patch 0031,
  M-mode userspace). Campaign data and the falsified regdb lever are in
  the git log.
- **Display bring-up latency** — two open-coded timers in the DSI glue
  replaced by a deterministic commit-tail hook (patch 0033): fbcon at
  ~5.5 s (was 7.2), video at ~5.7 s (was 10.0), verified over 23
  boots.
- **BMI270 intermittent init `-ENODEV`** — masked `INTERNAL_STATUS`
  poll + soft-reset/re-upload retries (patch 0013), verified 8/8 cold
  boots.

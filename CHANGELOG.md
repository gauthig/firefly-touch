# Changelog

Notable changes to firefly-touch. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

Scaffold complete and building clean for both panels. **Display, touch, and
the full UI are verified on both the plain ESP32-S3-Touch-LCD-4.3 (bench,
2026-08-05) and the target ESP32-S3-Touch-LCD-4.3B (COM11, 2026-08-08).**
**RV-C dimmer tap on/off is now verified working on the live coach**
(2026-08-09, `living_room` panel against a real G6) after fixing the
command-code/interlock bugs below. Hold-to-dim, the rest of the instance
map, and the other items in *Unverified* below are still unconfirmed.

### Added — Renogy MPPT solar monitoring (2026-08-28)

Issues #51–#53. The solar charge controller joins the batteries and the
Power Watchdog on the basement proxy's BLE links, broadcast to any panel
that wants it. **Confirmed on the coach 2026-08-28**: the proxy holds all
five BLE links at once and `main_cabinet` shows live PV watts/volts/amps,
battery volts and SOC, charge state and temperatures.

- **`components/renogy_solar`** (#52) — the controller has no radio of its
  own: the BT-1/BT-2 module is a transparent BLE-to-RS485 bridge, so this
  speaks plain **Modbus RTU**, not a vendor frame format. Protocol codec is
  pure C and host-tested (`host_test/`) like `rvc_protocol` and
  `jbd_bms_protocol`; the Bluedroid client is separate and never enters the
  simulator.
- ⚠️ **Discovery matches the module's EXACT advertised name**, not the
  `BT-TH-` prefix. In a campground a neighbouring rig's Renogy module
  advertises the same prefix, and connecting to it produces a perfectly
  healthy-looking link reporting somebody else's solar. Trailing whitespace
  is ignored — this coach's module pads its name to a fixed width.
- ⚠️ **A wrong Modbus device id produces silence, not an error.** The client
  therefore probes the known candidates (255 stand-alone, 16/17
  daisy-chained, 96/97 Communication-Hub) and logs whichever answers.
- **`components/ble_host` arbitrates the single GAP scan** (#51) — Bluedroid
  allows exactly one GAP callback per node, and both the Watchdog and the
  solar client discover by name. Clients register a matcher rather than
  scanning directly.
- **Solar telemetry over ESP-NOW** (#53). It fits the existing 16-byte
  envelope **exactly**, so the wire format did not change and older
  producers keep working. Broadcast even when unreachable, flagged offline,
  so a panel can tell "the controller dropped" from "the proxy is gone" —
  the battery contract, not the shore-power one.
- UI: `PANEL_BTN_SOLAR` readout on `main_cabinet` (its own SOLAR rail
  section, screen 4) and stacked under the battery bank on
  `bedroom_remote`. Temperatures are converted to °F **on the producer**, so
  one producer and several consumers cannot disagree about units.

### Fixed — LVGL heap exhausted on `main_cabinet` (2026-08-28)

`CONFIG_LV_MEM_SIZE_KILOBYTES` 64 → **128** in `sdkconfig.defaults`.

LVGL's pool is fixed at startup and cannot grow (`LV_MEM_POOL_EXPAND_SIZE`
is 0). `main_cabinet`'s four screens needed 78 KB just to build the UI and
peaked at 88 KB rendering, against a 64 KB cap. It still booted — much of
the cost is incurred lazily as each screen is first drawn — then wedged
after about seven nav-rail taps, with LVGL's renderer spinning on the failed
allocation instead of failing cleanly.

⚠️ **It presented as three unrelated faults.** `esp_lvgl_port` holds its lock
across all of `lv_timer_handler()`, so a stuck renderer also blocked the
ESP-NOW receive task and aged the battery and shore readouts out to blank,
while the task watchdog reported only that `taskLVGL` was pegging CPU 1.

⚠️ **The simulator could not catch this**: `sim/lv_conf.h` uses
`LV_STDLIB_CLIB`, an effectively unbounded allocator. Reproducing it needed
the sim temporarily switched to `LV_STDLIB_BUILTIN` with a matching
`LV_MEM_SIZE`. `bedroom_remote` measured 66,088 B peak — over the old cap
too — so it got the same change before being reflashed.

Measured budgets per device are recorded in
[docs/SYSTEM.md](docs/SYSTEM.md) → *Memory budget*.

### Added — MAIN CABINET panel on a Waveshare 7", side-nav UI (2026-08-23)

Issues #44–#48. A fourth panel (`main_cabinet`, index 3, source `0x83`),
and the first on a board other than the 4.3B.

- **New board: Waveshare ESP32-S3-Touch-LCD-7** (non-B), run **landscape**
  rather than rotated to portrait — which is what makes room for the rail.
  Verified on the real board: 16 MB flash / 8 MB PSRAM, same as the 4.3B, so
  `partitions.csv` and `sdkconfig.defaults` were unchanged despite the
  Waveshare wiki listing 8 MB for the product line.
- ⚠️ **CAN moves to GPIO20/19**, where the 4.3B has RS485, and those pins are
  shared with native USB behind CH422G EXIO5 (low = USB, high = CAN). The
  firmware raises it, so the board is flashed over UART. A board mismatch
  fails silently, so `BOARD` is **derived from `PANEL`** in the root
  `CMakeLists.txt` and validated by `tools/check_panels.py` against a new
  Board column in `panels/REGISTRY.md`. There is no `-DBOARD=` to forget.
- `components/board_4_3b` became **`components/board`** holding both boards
  behind one `board.h`; the CH422G driver split out into
  `components/ch422g`, shared by both.
- ⚠️ **Only the SOURCE selection is conditional in the board component;
  `REQUIRES` is constant.** IDF resolves the dependency graph in an early
  expansion pass that runs before the project's variables exist. A first
  attempt used two components that each registered empty unless selected;
  every `REQUIRES` silently evaluated to nothing and the build failed with
  "board.h: No such file" while the CMake looked correct.
- **Side-nav rail** (`PANEL_HAS_NAV_RAIL`), plus `PANEL_DEFAULT_SCREEN` and
  `PANEL_GRID_COLS`. Rail entries are ordinary `PANEL_BTN_SCREEN_SWITCH`
  defs and are matched to sections by target screen, so rail order is
  independent of screen index. `build_screen2_row()` was left untouched for
  the three installed panels rather than generalised.
- **`PANEL_BTN_LOCAL_TOGGLE`** — caption-flipping buttons that send
  nothing, for the grey/black dump valves and the gravity/macerator
  selector. Deliberately inert: the actuation isn't built, and they are
  coloured like any other on-state rather than with the warn colour, since
  an alarm colour would announce an open dump valve that doesn't exist.
- **`PANEL_BTN_LIGHT_MASTER`** — off sweeps every instance the state
  manager reports as on (reaching lights this panel has no button for); on
  applies the header's `PANEL_MASTER_ON[]` scene, because RV-C has no all-on
  command and **the G6's own LIGHT MASTER DGN has never been captured**.
  Direction is read fresh at tap time and the visual is primed at build
  time — without both, a tap in the first second after boot saw stale
  state and turned everything on while lights were already on.
- `panel_btn_def_t` gained `label_alt`; every panel header's button table
  moved to **designated initializers**, which ends the
  `-Wmissing-field-initializers` churn for any future field.
- **Three latent bugs fixed**, all of which only bite a CAN panel showing
  broadcast telemetry: the shore-power timer was created only
  `#if !PANEL_HAS_CAN`; the grey/black and battery scans looked only at
  `PANEL_BUTTONS_2`; and `idle_timer_cb` returned to screen 0 instead of
  the panel's default screen.
- `PANEL_WANTS_TELEMETRY` lets a CAN panel run ESP-NOW in the receive-only
  TELEMETRY role. `main_cabinet` needs it: the battery packs and the Power
  Watchdog are on the proxy's BLE links, which no CAN wiring reaches.
- Simulator sizes its window from the panel's **logical** resolution
  (landscape for a rail panel), and `--shot` gained
  `section:<LABEL>[,<LABEL>...]` for tapping a sequence — the only way to
  reach a control inside a section, since a hidden widget can't be
  hit-tested.

**Confirmed on the coach 2026-08-23.** RV-C CAN works on GPIO20/19, which
also confirms the EXIO5 USB/CAN mux polarity; the battery bank and shore
power arrive as ESP-NOW broadcasts from the proxy; Light Master on and off
behave as designed against real loads; and the display geometry and colours
are correct, confirming the RGB timings. Builds are clean for all four
panels and the proxy and the host tests pass.

Still open, by choice: whether the G6 has a real all-lights command on the
bus. The factory LIGHT MASTER rocker has never been sniffed, so the panel's
master stays synthesised until it is.

### Changed — battery BLE moved to the basement proxy (2026-08-23)

Issues #40–#42. The three JBD/Xiaoxiang packs are in the same basement bay
as the BLE proxy, so holding their links from a bedroom wall panel put a
steel bay door in the RF path and made one panel's radio carry three BLE
connections alongside ESP-NOW — for data any panel would eventually want.

- `bedroom_remote` no longer runs a BLE client at all. `PANEL_HAS_BLE_BATTERY`
  and its wiring were removed rather than left as an unselected option.
- The proxy now holds **four** BLE links (3 packs + the Power Watchdog) and
  broadcasts per-pack readings every 30 s.
- **Per-pack frames on the wire, not a pre-combined bank.** `jbd_bms_combine()`
  stays the single host-tested aggregator, and the panel's per-pack detail
  popup keeps real numbers. A configured-but-unreachable pack is broadcast
  flagged offline, so "this pack dropped" stays distinguishable from "the
  proxy is gone".
- New `components/ble_host`: shared Bluedroid bring-up plus GAP/GATTC
  callback fan-out. Bluedroid holds exactly one GAP and one GATTC callback
  per node and a second registration *replaces* the first without erroring,
  so two clients on one node would silently fight; GATTC app IDs are also one
  flat namespace, and both clients had been using 0.
- `jbd_bms_client.c`'s `try_connect()` now selects on
  `CONFIG_BT_BLE_50_FEATURES_SUPPORTED` — the classic ESP32 is BLE 4.2 and
  needs `esp_ble_gattc_open()`, not the S3's `esp_ble_gattc_enh_open()`.
- The battery telemetry struct is sized to fit the existing 16-byte union
  member, pinned by a `_Static_assert`, so `sizeof(espnow_telem_frame_t)`
  is unchanged and `mid_coach`'s tank broadcasts keep parsing on an updated
  panel without reflashing it.
- New `ui_on_battery_status()` entry point makes the UI source-agnostic; the
  simulator feeds the same path, so `sim/stubs/jbd_bms_client.h` is gone and
  `jbd_bms_combine()` runs for real in the sim.

Bench-verified at the coach 2026-08-23: all three packs and the Watchdog
connect from the proxy, and `bedroom_remote` shows the combined bank from
broadcasts alone.

### Fixed — battery current sign convention confirmed (2026-08-23)

Positive = charging, negative = discharging, observed in both directions on
real packs. This had stood as an assumption since the protocol work: the
committed regression frame carries a zero current field and so could never
settle it. The long-standing `TODO(bench)` is closed.

### Removed — OTA update feature (2026-08-16)

- **All OTA (over-the-air) update code has been removed.** Bench testing
  against the target network showed it is 5G-only with WPA3 + PMF
  (Protected Management Frames) required. The ESP32-S3's Wi-Fi radio is
  WPA2-only and cannot associate — authentication fails
  (reason=202/AUTH_FAIL) even with strong RSSI and correct credentials.
  This is a hardware/protocol incompatibility, not a code bug, and will
  not be worked around (e.g. by weakening PMF).
- Removed `components/ota_update/` in full (manifest parser + host test),
  the OTA rollback-confirmation code in `main/main.c`, and
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` from `sdkconfig.defaults`.
- Reverted `partitions.csv` from the dual-OTA layout (`otadata`/`ota_0`/
  `ota_1`) back to a single `factory` app partition, since the dual-slot
  layout only existed to support a future OTA path that will now never
  exist.
- **Decision: firmware updates are USB-only, permanently**, for both the
  Wi-Fi incompatibility above and security reasons (no network attack
  surface for firmware updates). See `CLAUDE.md` and
  `docs/FLASHING.md#firmware-updates-after-installation`.

### Changed

- **Panel nomenclature standardized (2026-08-16).** The living room panel
  was renamed from `living_room` to **`mid_coach`** (on-screen name stays
  "MID COACH") and the remote panel from `living_room_remote` to
  **`bedroom_remote`** (on-screen name now "BED REMOTE"). This covers the
  panel headers (`panels/mid_coach.h`, `panels/bedroom_remote.h`), build
  directories (`build_mid_coach`, `build_bedroom_remote`), all build
  configs, docs, registry, and memory files. No hardware assignments,
  button layouts, instances, indices, or source addresses changed.
- **New naming convention documented:** a panel whose ID ends in `_remote`
  is an **ESP-NOW device** (no RV-C CAN wiring) that reports to the Mid
  Coach bridge; a panel without `_remote` is **hardwired to the RV-C CAN
  bus**; `mid_coach` is the ESP-NOW router/bridge between the RV-C bus and
  all remotes. See `panels/REGISTRY.md`, `docs/FLASHING.md`, and
  `CLAUDE.md`.
- **Idle auto-dim timeout extended from 60 s to 5 min (300 000 ms)** —
  the 1-minute timeout proved too aggressive for normal use.
- **Diagnostic logging added to `idle_timer_cb`** — logs `inactive_ms` every
  second to confirm the inactivity counter is climbing correctly and to catch
  any phantom touch activity that would keep resetting it. Will be removed
  once the idle-dim path is verified on the 4.3B.

### Added — 4.3B bring-up (2026-08-08)

- **First successful flash to the Waveshare ESP32-S3-Touch-LCD-4.3B
  (Version B, COM11, MAC 44:1b:f6:8d:00:7c).** This is the actual in-wall
  deployment target (7–36 V DC input, TJA1051 CAN transceiver). Build
  `living_room` panel confirmed to flash and boot via `idf.py flash`.

### Fixed — G6 load-latchup / wrong RV-C command codes (2026-08-08, coach test)

First live-coach test showed loads responding ~1 s late, working exactly
once, then ignoring **all** panels (factory ones included) until the G6 was
power-cycled. Root causes, all in our TX frames — cross-checked against
rvc-proxy `dc_dimmer.pl`, CoachProxy `command_dc_dimmer.js`, and the
rvc2hass `rvc-spec.yml` (all proven against real coaches):

- **Command enum was misnumbered.** 17/18 were used as ramp up/down; the
  spec says 17 = ramp *brightness*, 18 = ramp *toggle*, 19/20 = ramp
  up/down (also lock/unlock are 33/34 and flash 49/50, not 21/22/33/34).
  Hold-to-dim therefore spammed "ramp brightness to 0xFF" ~10×/s — the
  suspected open ramp session that wedged the G6's dimmer engine for that
  load. Enum renumbered to the spec values.
- **Interlock byte (byte 5) sent as 0xFF.** Spec: bits 0-1 = interlock,
  `00` = none. All proven implementations send 0x00; we now do too.
- **ON/OFF sent level 0xFF ("no change").** Proven frames carry an explicit
  desired level (0xC8 = 100 %); taps now do the same.

**Confirmed fixed on the coach (2026-08-09)**, after a G6 power cycle:
tap on/off is responsive and repeatable, no further latchup observed.

### Changed — living room panel, second bring-up round (2026-08-08)

- **Portrait orientation**: display now runs rotated 90° CW via LVGL
  `sw_rotate` (touch coordinates transform automatically); verified on
  hardware. `full_refresh` must NOT be combined with `sw_rotate` on this
  esp_lvgl_port version — see the gotcha below.
- **Tap-to-toggle reliability**: dimmer buttons now send an explicit ON/OFF
  (not TOGGLE) and arm an ~800 ms confirm timer waiting for the
  `DC_DIMMER_STATUS_3` echo, resending once if it doesn't arrive. Visual
  state remains driven only by real status frames — an optimistic
  immediate-flip approach was tried and reverted for violating that
  invariant. RV-C has no command-ack DGN, so this status-echo-with-retry is
  the closest equivalent.
- **"ODS Sofa Sconce" button renamed to "Sofa Sconce"** in `panels/living_room.h`.
- Hold-to-dim (continuous ramp while held, matching standard RV-C behavior)
  was left as designed; `LV_OBJ_FLAG_PRESS_LOCK` restored to its LVGL
  default so small finger drift doesn't cancel the long-press.

### Fixed — hardware bring-up

- **White-blink display bug:** `avoid_tearing` (two PSRAM framebuffers) was
  enabled without `direct_mode`, so LVGL rendered partial-mode dirty regions
  into alternating framebuffers — visible as the UI alternating with a full
  white frame at the 500 ms CAN-health timer cadence and garbling on touch.
  One flag in `board_4_3b.c`; verified stable on hardware.
- Bring-up gotchas recorded in CLAUDE.md: the RESET-button/download-mode
  trap, 5 V supply sizing (undersized USB power mimics firmware bugs), the
  plain-4.3 vs 4.3B power/battery connector difference, and SpotPear pin
  evidence (plain 4.3: CAN = GPIO 19/20, RS-485 = 15/16) against the
  unverified TWAI 15/16 assignment.

### Added — project foundation

- ESP-IDF v5.5.5 project targeting ESP32-S3 (Waveshare ESP32-S3-Touch-LCD-4.3B),
  LVGL 9.5 via `esp_lvgl_port`, pinned by a committed `dependencies.lock`.
- FreeRTOS task architecture with explicit core pinning: `twai_rx` (prio 12),
  `twai_tx` (prio 11) and `state_manager` (prio 9) on core 0; the LVGL task on
  core 1. Touch handlers never call TWAI directly — they post to a TX queue.
- **Status-driven UI invariant:** button state changes only in response to
  `DC_DIMMER_STATUS_3` frames from the bus, never from locally sent commands.
  This is what keeps panels in sync with factory switches and the Firefly app.

### Added — RV-C protocol

- `rvc_protocol` component: 29-bit ID pack/unpack (priority 6, DGN bits 8–24,
  source address bits 0–7), `DC_DIMMER_COMMAND_2` (0x1FEDB) encode with the
  standard command enum, `DC_DIMMER_STATUS_3` (0x1FEDA) decode. Pure C with no
  ESP dependencies so it stays host-testable.
- Host unit tests covering ID round-trips, known-good frames, level clamping
  and short-frame handling. Pass under `-Wall -Wextra -Werror`.
- Sniffer mode (`CONFIG_FIREFLY_SNIFFER_MODE`) logging every received frame —
  raw ID, DGN, source address, and data bytes — for bench verification.

### Added — panels

- Two panel configurations selected at build time via `-DPANEL=`:
  `living_room` (replaces Entegra SW2-E8) and `ent_center` (replaces SW4-E1).
- Shared dimmer-button widget: tap to toggle, press-and-hold to ramp,
  brightness bar driven by the reported operating level. Multi-instance
  buttons send explicit on/off to every member so grouped loads can't drift.
- Dark night theme, CAN-health indicator, and idle auto-dim (initially 60 s,
  extended to 5 min — see Changed above) whose waking touch is absorbed
  rather than passed to the button underneath.
- `panels/REGISTRY.md` as the canonical `PANEL_INDEX` → source-address
  allocation record, `panels/TEMPLATE.h` for new panels, and
  `tools/check_panels.py` enforcing unique indices and registry sync.

### Added — OTA readiness

- Custom dual-OTA partition table (`partitions.csv`): two 4 MB app slots plus
  ~8 MB `storage`, on 16 MB flash. Reserved **before** panels are installed —
  changing the layout later would force USB reflashing of every wall-mounted
  panel.
- Bootloader rollback enabled. `app_main()` calls
  `esp_ota_mark_app_valid_cancel_rollback()` only after display, touch, UI and
  the RV-C tasks are up, so a bad OTA reverts itself on the next reset.
- Wi-Fi transport is **not implemented** — the layout exists so adding it later
  needs no repartitioning.

### Added — tooling

- PC simulator (`sim/`) compiling the real UI sources against the same LVGL the
  firmware uses, with a fake bus that echoes status frames. Mouse acts as
  touch; supports headless screenshots.
- GitHub Actions CI: validates the panel registry, runs host protocol tests,
  and builds every panel with a matrix derived from the registry, so new panels
  get coverage automatically. Uploads flashable artifacts.
- `README.md`, `docs/FLASHING.md` (per-device upload, OTA, adding a panel), and
  `CLAUDE.md` (architecture, DGN tables, pinout, gotchas).
- Licensed under the MIT License (Copyright (c) 2026 Garrett).

### Unverified — read before connecting to a live bus

- **TWAI TX/RX GPIO 15/16 has not been checked against the 4.3B schematic.**
  Wrong pins can disturb the coach's RV-C bus.
- The entire RV-C instance map came from factory switch legends, not from the
  bus. Confirm with a sniffer build.
- `SECURITY P+H` (instances 44/45) may be a different load type or a scene
  rather than plain dimmer instances.
- `PANEL LIGHTS` ("PL1") DGN is unknown; the button currently logs its press
  and cycles the local LCD backlight only.
- Source addresses `0x80`+ are assumed free — sniff for collisions first.
- RGB panel timings are from Waveshare's 4.3 demo; diff against the current
  4.3B release at bring-up.

### Known hardware limitation

- The 4.3B backlight is gated by CH422G EXIO2, which is on/off only — there is
  no PWM path. Intermediate dimming is emulated with a translucent LVGL
  overlay until a PWM-capable route is found.

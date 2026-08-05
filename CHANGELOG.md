# Changelog

Notable changes to firefly-touch. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

Scaffold complete and building clean for both panels. **Nothing in this
release has run on real hardware yet** — see *Unverified* below before
connecting a panel to the coach.

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
- Dark night theme, CAN-health indicator, and 60-second idle auto-dim whose
  waking touch is absorbed rather than passed to the button underneath.
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
- Released into the public domain under The Unlicense.

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
- The CI workflow has not executed yet; it validates on first push.

### Known hardware limitation

- The 4.3B backlight is gated by CH422G EXIO2, which is on/off only — there is
  no PWM path. Intermediate dimming is emulated with a translucent LVGL
  overlay until a PWM-capable route is found.

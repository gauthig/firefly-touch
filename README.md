# firefly-touch

[![build](https://github.com/gauthig/firefly-touch/actions/workflows/build.yml/badge.svg)](https://github.com/gauthig/firefly-touch/actions/workflows/build.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Replacement **touchscreen wall panels** for a 2019 Entegra Aspire 44W with a
Firefly Integrations **G6A multiplex system**.

The coach's factory switch panels are fixed-function membrane switches. This
project replaces them with 4.3" capacitive touchscreens that speak the coach's
native **RV-C** protocol directly on the CAN bus — no hub, no gateway, no
cloud. Each panel is an **independent peer node**: it transmits light commands
and listens for status frames, so it stays in sync with the factory switches
and the Firefly app automatically.

Beyond lights, the panels also show **tank levels**, a combined **battery
bank** readout for three parallel LiFePO4 packs, and **shore power** from a
Hughes Power Watchdog — the latter two reached over BLE and shared to every
panel by ESP-NOW broadcast.

| | |
|---|---|
| ![bedroom_remote battery bank](docs/images/bedroom-remote-battery.png) | ![mid_coach lights](docs/images/mid-coach-lights.png) |

📖 **[All screen samples →](docs/SCREENS.md)** · 🔌 **[System architecture and
installed equipment →](docs/SYSTEM.md)**

## How it works

- **Status-driven UI.** A button lights up only when a `DC_DIMMER_STATUS_3`
  frame from the bus says the load is on — never because you pressed it. Flip
  a factory switch and this panel updates; press here and the factory panel's
  LED updates. No master, no polling.
- **Tap to toggle, press-and-hold to dim.** Holding sends RV-C ramp commands
  while held and a stop on release.
- **Multi-instance buttons.** One button can drive several RV-C instances
  (e.g. SIDE CEILING = 30 + 31); it shows ON if any member is on and sends
  explicit on/off to all members so they can't drift out of sync.
- **Runs on the bus at night.** Dark theme, idle auto-dim at 120 s and
  backlight-off at 300 s, and the touch that wakes the screen doesn't
  trigger the button underneath.
- **Data it isn't wired to.** A headless ESP32 in the basement bay holds
  every BLE link in the coach — the three battery packs and the shore-power
  monitor, all of which sit in that bay — and re-broadcasts their readings
  over ESP-NOW, so any panel can display them without a connection of its
  own. No panel runs a BLE stack.

## Hardware

Six nodes: four touchscreen panels, one headless BLE proxy, and one headless
valve controller. They reach the coach over RV-C CAN and each other over
ESP-NOW. Everything reached over BLE — the battery packs, the shore-power
monitor and the solar charge controller, all of which live in the basement
bay — is held by the proxy in that bay and broadcast to the panels.

| Node | Board | Talks |
|---|---|---|
| `mid_coach` | Waveshare ESP32-S3-Touch-LCD-4.3B | RV-C CAN + ESP-NOW bridge |
| `ent_center` | Waveshare ESP32-S3-Touch-LCD-4.3B | RV-C CAN |
| `bedroom_remote` | Waveshare ESP32-S3-Touch-LCD-4.3B | ESP-NOW only |
| `main_cabinet` | Waveshare ESP32-S3-Touch-LCD-7 | RV-C CAN + ESP-NOW telemetry (listen only) |
| Bluetooth proxy basement | ESP32-D0WD-V3, 4 MB | BLE (3 battery packs + Power Watchdog + Renogy solar) + ESP-NOW broadcast |
| `valve_node` *(planned)* | Waveshare ESP32-S3-ETH-8DI-8RO | ESP-NOW unicast — drives the two dump valves |

Panel boards are ESP32-S3-WROOM-1 with 16 MB flash / 8 MB octal PSRAM, GT911
touch, CH422G expander, onboard TJA1051 CAN transceiver and 7–36 V input.
The 4.3B panels run their 800×480 LCD rotated to portrait; `main_cabinet`'s
7" runs landscape, which is what gives its side-nav rail room to live.

⚠️ **The two boards put CAN on different pins** — 4.3B on GPIO15/16, the 7"
on GPIO20/19, where the 4.3B has RS485 — and on the 7" those pins are muxed
against native USB, so it is flashed over its UART port. A board mismatch
fails silently, so `BOARD` is derived from `PANEL` in the root
`CMakeLists.txt` and validated by `tools/check_panels.py`; there is no
`-DBOARD=` to get wrong.

The two headless nodes are not panels: they carry no display, no LVGL and no
`PANEL_INDEX`, and each is its own ESP-IDF project pulling shared components
individually.

📖 **Full equipment list, protocols and architecture diagram:
[docs/SYSTEM.md](docs/SYSTEM.md)** · **Dump-valve build spec:
[docs/DRAINMASTER-VALVES.md](docs/DRAINMASTER-VALVES.md)**

## Repository layout

```
firefly-touch/
├── main/                     # panel app: tasks, state, UI
├── components/
│   ├── rvc_protocol/         # RV-C encode/decode    ┐ pure C,
│   ├── jbd_bms/              # battery BMS + bank agg ├ host-testable
│   ├── hughes_watchdog/      # Power Watchdog decode ┘ (host_test/)
│   ├── espnow_link/          # panel-to-panel link + telemetry broadcast
│   ├── board_4_3b/           # Waveshare bring-up: RGB, CH422G, GT911, TWAI
│   └── ui_common/            # theme + shared widgets
├── panels/                   # ← per-panel config, selected at build time
│   ├── REGISTRY.md           #   canonical index → source-address allocation
│   └── TEMPLATE.h            #   copy this to add a panel
├── proxy/                    # basement BLE proxy — SEPARATE project,
│                             #   classic ESP32 target, no display
│                             #   (valves/ will join it: dump-valve relay
│                             #    controller, also headless — see
│                             #    docs/DRAINMASTER-VALVES.md)
├── sim/                      # PC simulator (see the UI without hardware)
├── tools/check_panels.py     # enforces unique indices + registry sync
├── docs/                     # SYSTEM.md, SCREENS.md, FLASHING.md,
│                             #   DRAINMASTER-VALVES.md, images
├── .github/workflows/        # CI: lint, host tests, every panel + the proxy
└── CLAUDE.md                 # architecture, DGN tables, pinout, TODOs
```

**One codebase, many panels.** A panel is just a header in `panels/` defining
its name, index, and button grid. Adding a third panel is one new header plus
a build flag — no code changes. Copy `panels/TEMPLATE.h`, claim the next index
from [`panels/REGISTRY.md`](panels/REGISTRY.md), and run
`python tools/check_panels.py`. Full procedure:
[docs/FLASHING.md](docs/FLASHING.md#adding-a-new-panel).

**Updates after installation.** No OTA — the ESP32-S3's Wi-Fi radio is
WPA2-only and can't associate to a WPA3+PMF network, so updates stay
USB-only by design (single-app `partitions.csv`, no Wi-Fi/OTA code). See
[docs/FLASHING.md](docs/FLASHING.md#firmware-updates-after-installation).

## Toolchain setup

You need **ESP-IDF v5.3+** (developed against v5.5.5). Everything else —
compiler, CMake, Ninja, Python env — comes from ESP-IDF's own installer.
Install both targets: `esp32s3` for the panels, `esp32` for the proxy.

```powershell
git clone --branch v5.5.5 --depth 1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git C:\esp\esp-idf
C:\esp\esp-idf\install.ps1 esp32s3,esp32
```

```bash
# Linux / macOS
git clone --branch v5.5.5 --depth 1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3,esp32
```

Then **in every new terminal**, activate the environment before using
`idf.py` — it is deliberately not on your global PATH:

```powershell
. C:\esp\esp-idf\export.ps1     # or: . ~/esp/esp-idf/export.sh
```

The [ESP-IDF VS Code extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
wraps the same toolchain and handles the environment for you.

A **host C compiler** is needed only for the unit tests and the PC simulator,
not for firmware — on Windows, `winget install BrechtSanders.WinLibs.POSIX.UCRT`.
Managed components (LVGL 9.5.0, esp_lvgl_port 2.8.0, esp_lcd_touch_gt911
1.2.0) download on first build, pinned by `dependencies.lock`.

## Build, flash, monitor

Each panel builds into its own directory. **`PANEL` selects which panel's
buttons and identity are compiled in** — see
[docs/FLASHING.md](docs/FLASHING.md) for the full per-device procedure.

```powershell
idf.py -B build_mid_coach -DPANEL=mid_coach -p COM5 flash monitor
```

The basement proxy is a **separate project** on a different chip, so it
builds on its own:

```powershell
idf.py -C proxy -B proxy/build set-target esp32
idf.py -C proxy -B proxy/build -p COM4 flash monitor
```

> ⚠️ Panels that use ESP-NOW or BLE keep real peer MACs and keys in their
> per-build `sdkconfig`, which is **not** in git. Pass
> `-D SDKCONFIG=build_<panel>/sdkconfig` on every invocation, and never
> regenerate that file — see [docs/FLASHING.md](docs/FLASHING.md).

Verify which firmware a board is running — it prints its identity at boot and
shows the name in the status bar:

```
I (312) main: firefly-touch panel 'MID COACH' (index 0, RV-C source addr 0x80)
```

## PC simulator

See and click the real UI without flashing anything. It compiles the actual
`ui.c` and widget code against the same LVGL version the firmware uses, with a
fake RV-C bus that echoes status frames back.

```powershell
cd sim
.\build.ps1 -Run                              # mid_coach
.\build.ps1 -Panel bedroom_remote -Run
.\build.ps1 -Panel bedroom_remote -Shot p.bmp -Screen2   # headless capture
```

Mouse = touch: click to toggle, click-and-hold to ramp. The window is
480×800, matching the firmware's logical (post-rotation) resolution. Fake
tank, battery and shore-power feeds make every state reachable, including a
pack dropping offline.

Requires a host C compiler (above); SDL2 downloads into `sim/third_party/`
on first build. Run one `idf.py build` first so `managed_components/` exists.

See [docs/SCREENS.md](docs/SCREENS.md) for what each screen looks like.

## Tests

Every protocol codec is pure C with no ESP-IDF dependency, so it runs
natively on your machine — RV-C frames, the JBD battery protocol and bank
aggregation, and the Power Watchdog decode. CI runs all three.

```powershell
cd components/rvc_protocol/host_test
gcc -Wall -Wextra -Werror -I../include ../rvc_protocol.c test_rvc.c -o test_rvc && ./test_rvc
```

Same pattern in `components/jbd_bms/host_test` and
`components/hughes_watchdog/host_test` (both need `-lm`). Real captured
device frames are committed as regression vectors, so a "simplification"
that breaks a byte offset fails loudly.

## Releases

Pushing a `v*.*.*` tag runs the full pipeline and publishes a GitHub Release
with a zip per panel (app, bootloader, partition table, `flash_args`) for
flashing via [docs/FLASHING.md](docs/FLASHING.md). No OTA and no deploy
target — these are wall panels updated over USB.

## Bench verification

RV-C instance numbers came from the factory switch legends and **must be
confirmed on the real bus**. Sniffer mode logs every frame (raw ID, DGN,
source address, data bytes): `idf.py menuconfig` → *Firefly Touch Panel* →
*RV-C sniffer mode*.

## Status

Confirmed on the coach: RV-C lighting control against a real G6, tank levels
from the SeeLevel, the ESP-NOW bridge and remote panel, all three battery
packs over BLE with the combined bank readout, and the Power Watchdog with
values matching the unit's own display.

`main_cabinet` (the 7" side-nav panel) was confirmed on the coach
2026-08-23: RV-C CAN works on the 7" board's GPIO20/19 pins, the battery
bank and shore power arrive as ESP-NOW broadcasts from the proxy, Light
Master on/off behaves as designed, and the display geometry and colours are
correct. That settles both bring-up unknowns — the RGB timings and the
EXIO5 USB/CAN mux polarity.

Known open items, all tracked as `TODO(bench)` in code:

- **The G6's own LIGHT MASTER command**, if it has one. The panel's master
  is synthesised (sweep the on-lights off; apply a scene for on) because
  the factory rocker's DGN has never been captured. Worth sniffing the
  rocker to find out whether a single real frame exists.
- **Power Watchdog byte offsets** came from public reverse engineering
  rather than a capture from this unit; the values check out against its
  display, but the client logs raw packets for confirmation.
- The rest of the instance map, the SECURITY (patio/hitch) button, and the
  PANEL LIGHTS (PL1) DGN.

[CLAUDE.md](CLAUDE.md) holds the architecture, DGN tables, pinout and the
full TODO list. [CHANGELOG.md](CHANGELOG.md) records what's built, what's
verified, and what is still assumption.

## License

[MIT](LICENSE) — Copyright (c) 2026 Garrett.

RV-C is an open standard published by the RV Industry Association. This
project is not affiliated with or endorsed by Firefly Integrations, Entegra
Coach, Waveshare, Vatrer, Garnet, or Hughes Autoformers.

> No warranty. This firmware transmits on a live vehicle control bus; you are
> responsible for verifying it against your own coach before connecting it.

# firefly-touch

Replacement touchscreen wall panels for a 2019 Entegra Aspire 44W with a
Firefly Integrations G6A multiplex system. Each panel is an **independent peer
node** on the coach's RV-C bus (CAN 2.0B, 250 kbps, 29-bit extended IDs).
There is no hub or gateway — the bus is the shared state mechanism.

Hardware: Waveshare **ESP32-S3-Touch-LCD-4.3B** (ESP32-S3-WROOM-1-N16R8,
16 MB flash / 8 MB octal PSRAM, 4.3" 800x480 RGB LCD, GT911 touch on I2C,
CH422G IO expander, TJA1051 CAN transceiver, 7–36 V input).

Docs for humans: [README.md](README.md) (overview, toolchain install) and
[docs/FLASHING.md](docs/FLASHING.md) (per-device upload, panel identity
matrix, adding a panel). Keep all three in sync when things change.

## Build / flash / monitor

Requires ESP-IDF **v5.3+**; developed and verified against **v5.5.5** with
LVGL 9.5.0, esp_lvgl_port 2.8.0, esp_lcd_touch_gt911 1.2.0 (pinned in the
committed `dependencies.lock`).

**Activate the environment first in every new shell** — `idf.py` is not on the
global PATH:

```
. C:\esp\esp-idf\export.ps1
```

```
idf.py -B build_living_room -DPANEL=living_room build
idf.py -B build_ent_center  -DPANEL=ent_center  build
idf.py -B build_living_room -DPANEL=living_room -p COM5 flash monitor
```

Use one build dir per panel (PANEL is cached; switching values in a shared
build dir requires `fullclean`). Valid PANEL values = basenames of headers in
`panels/`. Panel identity: `PANEL_INDEX` → RV-C source address `0x80 + index`;
`living_room` = 0/0x80, `ent_center` = 1/0x81. Never reuse an index.

Sniffer mode (log every RV-C frame — how the instance map gets verified):
`idf.py menuconfig` → *Firefly Touch Panel* → *RV-C sniffer mode*, or add
`CONFIG_FIREFLY_SNIFFER_MODE=y` to `sdkconfig.defaults` for a bench build.

Host unit tests (pure-C protocol code, any host compiler):

```
cd components/rvc_protocol/host_test
gcc -Wall -Wextra -I../include ../rvc_protocol.c test_rvc.c -o test_rvc && ./test_rvc
```

## PC simulator (see what the LCD looks like without hardware)

`sim/` builds the **real UI sources** (`main/ui/ui.c`, `ui_common`,
`rvc_protocol`, the panel headers) into a native Windows exe using the same
LVGL version as the firmware (from `managed_components/` — run an `idf.py`
build once first to populate it). ESP-specific headers are shadowed by
`sim/stubs/`; `sim_stubs.c` fakes the RV-C bus by echoing STATUS_3 back, so
the status-driven UI path is exercised exactly like on hardware.

```
cd sim
.\build.ps1 -Run                    # living_room, interactive window
.\build.ps1 -Panel ent_center -Run
.\build.ps1 -Shot preview.bmp       # headless screenshot, then exits
```

Mouse = touch: click to toggle, click-and-hold to ramp. Requires the WinLibs
gcc (winget) and cmake/ninja (auto-sourced from ESP-IDF's export.ps1). SDL2
lives in `sim/third_party/` (gitignored; build.ps1 error tells you the
download if missing).

`sim_stubs.c` also fakes `TANK_STATUS` on `living_room` (a periodic sweep
through FRESH/GREY/BLACK percentages, `tank_sweep_timer_cb`) so the wave
gauges and the header's Grey-Black OK/Warn/FULL readout are visible without
real hardware. `--shot <file> screen2` (`main_sim.c`) taps the TANK LEVELS
button and runs real time forward briefly before snapshotting so both
timers get a chance to fire; `SIM_TANK_START_TICK=<n>` (env var) jumps the
sweep straight to a specific point (e.g. a FULL/blink frame) for a
one-off capture instead of waiting on it.

## Architecture

One codebase, many panels: `panels/<name>.h` (selected at build time via
`-DPANEL=`) defines `PANEL_NAME`, `PANEL_INDEX`, and the `PANEL_BUTTONS[]`
grid layout. RV-C source address = `0x80 + PANEL_INDEX`. A panel additionally
sets `PANEL_HAS_CAN 0` if it has no CAN wiring at all (relays to a bridge
panel over ESP-NOW instead — see *ESP-NOW remote-panel bridge* below) or
`PANEL_IS_BRIDGE 1` if it is that bridge; both default to the normal
CAN-connected case in `main/panel_config.h`.

### Task map

| Task           | Core | Prio | Role |
|----------------|------|------|------|
| `twai_rx`      | 0    | 12   | blocks on `twai_receive`, stamps bus liveness, sniffer logging, decodes `DC_DIMMER_STATUS_3`, posts to status queue |
| `twai_tx`      | 0    | 11   | drains TX queue → `twai_transmit`; bus-off recovery. Nothing else ever blocks on the bus |
| `espnow_rx`    | 0    | 10   | (only if `PANEL_IS_BRIDGE` or `!PANEL_HAS_CAN`) drains ESP-NOW recv queue, invokes the registered cmd/status callback |
| `state_mgr`    | 0    | 9    | owns instance→{level,on} table; on change calls `ui_on_status()` under the LVGL lock, and the registered ESP-NOW status sink if any |
| LVGL (port)    | 1    | 4    | rendering + touch; button callbacks only enqueue via `bridge_enqueue_dimmer_cmd()` |

**Invariant:** icon/button visual state is driven ONLY by status frames from
the bus (or, on a remote panel, status relayed from the bridge's bus) —
never by locally sent commands. That keeps panels in sync with the factory
switches and the Firefly app.

Data flow (CAN-connected panel): touch → `ui_dimmer_button` event →
`panel_send_cb` → `bridge_enqueue_dimmer_cmd()` → `twai_enqueue_dimmer_cmd()`
→ TX queue → `twai_tx` → bus …then… bus → `twai_rx` → status queue →
`state_mgr` → `ui_on_status()` → widget. On a `PANEL_HAS_CAN 0` remote panel,
`bridge_enqueue_dimmer_cmd()` sends an ESP-NOW frame to the bridge instead of
enqueuing locally, and status arrives the same way in reverse (see below).

### Components

- `components/rvc_protocol` — pure C (no ESP deps): 29-bit ID pack/unpack,
  DGN encode/decode. Host-testable (`host_test/`).
- `components/board_4_3b` — Waveshare 4.3B bring-up: RGB timings, CH422G
  (custom minimal driver, `ch422g.c`), GT911, esp_lvgl_port glue, TWAI init.
  RGB timing / pin values follow Waveshare's published demo — **merge points
  are marked in `board_4_3b.c`; diff against the current Waveshare demo when
  bringing up hardware, don't invent timings.**
- `components/ui_common` — theme (`ui_theme`), shared dimmer-button widget
  (`ui_dimmer_button`), panel-config types (`panel_def.h`). Buttons show
  label text only (no icon glyph); background swaps dark blue -> light blue
  between off/on (see *UI* section below).
- `components/espnow_link` — ESP-NOW transport for a remote panel with no
  CAN wiring (see *ESP-NOW remote-panel bridge* below). No dependency on
  `main/`; mirrors `dimmer_cmd_msg_t`/`dimmer_status_msg_t` as its own
  ESP-free-of-`main` structs.

## ESP-NOW remote-panel bridge

`living_room_remote` (`panels/living_room_remote.h`) has no CAN wiring — it
relays button taps to `living_room` (`PANEL_IS_BRIDGE 1`) over ESP-NOW, and
`living_room` relays real `DC_DIMMER_STATUS_3` changes back to it, using the
same non-blocking, drop-if-full contract `twai_enqueue_dimmer_cmd()` already
had. v1 scope: exactly one remote per bridge, one fixed ESP-NOW peer (MAC +
PMK/LMK) configured entirely through Kconfig at build time — no runtime
pairing, no mesh. See `docs/FLASHING.md` → *ESP-NOW remote panel* for the
pairing/flashing procedure.

- `main/panel_config.h` — `PANEL_HAS_CAN` / `PANEL_IS_BRIDGE` defaults and
  the `PANEL_IS_BRIDGE` ⇒ `PANEL_HAS_CAN` guard.
- `main/bridge_tx.c` — the one place `ui.c`'s `panel_send_cb` calls into;
  resolves to `twai_enqueue_dimmer_cmd()` or `espnow_link_send_cmd()` at
  build time depending on `PANEL_HAS_CAN`, so `ui.c` never branches on role.
- `main/state_manager.c` — `state_manager_register_status_sink()` lets the
  bridge forward every real status change over ESP-NOW alongside the normal
  `ui_on_status()` call; `state_manager_for_each_known()` backs a 30 s
  periodic full resync (`main/main.c`, `bridge_resync_timer_cb`) so a
  remote panel that just booted or missed a broadcast self-heals instead of
  showing stale state.
- `main/main.c` wiring: `PANEL_IS_BRIDGE` registers the status sink and the
  ESP-NOW command-rx callback (which just calls `twai_enqueue_dimmer_cmd()`,
  exactly like a local button press); `!PANEL_HAS_CAN` registers a
  status-rx callback that calls `ui_on_status()` directly — no local
  state_mgr, the remote panel is purely a display of what the bridge
  reports.
- Link-health dot (remote panel's status bar, same spot/colors as the
  CAN-health dot): `espnow_link_healthy()` vs `state_manager_bus_healthy()`,
  selected by `PANEL_HAS_CAN` in `ui.c`'s `link_health_timer_cb`.
- Security: `esp_now_set_pmk()` + per-peer LMK from
  `CONFIG_FIREFLY_ESPNOW_PMK`/`CONFIG_FIREFLY_ESPNOW_LMK` — **change both
  from their placeholder defaults before deploying**, these frames actuate
  real loads.

**Bench-verified 2026-08-13** on real hardware (`living_room` on COM16,
`living_room_remote` on COM11): both boards boot, initialize ESP-NOW, log
the correct peer MAC for each other after pairing, and the full round trip
works — tap on the remote actuates the real load and confirms via the
status echo, and the remote's display updates when the load is toggled from
`living_room`'s own button. Known v1 limits, not bugs:
status broadcasts are best-effort with no delivery ack (mitigated by the 30 s
resync, same as RV-C's own status frames having no ack); exactly one
remote/bridge pair; no runtime pairing UI.

⚠️ **`sdkconfig` is shared at the repo root across every `-B build_<panel>`
directory** — discovered while pairing the two boards above. Only `PANEL`
(the C source selection) is a per-build-dir CMake cache var; Kconfig
settings like the ESP-NOW peer MAC/PMK/LMK are not, and both panels need
different values. Give each of `living_room`/`living_room_remote` its own
sdkconfig with `-D SDKCONFIG=build_<panel>/sdkconfig` on every `idf.py`
invocation (configure, menuconfig, build, flash) — see
`docs/FLASHING.md` → *ESP-NOW remote panel*. Skipping it on one command
silently edits the shared root `sdkconfig` and the other panel's next build
picks up whatever was last written there.

**Remote panel button layout is independent of `living_room`'s own buttons.**
Since `bridge_tx.c`/`state_manager`'s status sink forward whatever instance
they're given regardless of what's in `living_room.h`'s `PANEL_BUTTONS[]`,
the remote doesn't need to mirror the bridge's local button set — as of
2026-08-13 `panels/living_room_remote.h` was reprogrammed to a bedroom/
bathroom-focused layout (instances 17, 25, 35, 46 left column; 18, 13, 21,
Panel Lights right column) unrelated to `living_room`'s living-room-focused
buttons. See [docs/instance_map.yaml](docs/instance_map.yaml) for the full
RV-C instance map used to pick these.

## RV-C protocol

| DGN | Name | Direction | Use |
|-----|------|-----------|-----|
| `0x1FEDB` | DC_DIMMER_COMMAND_2 | TX | instance, group(0xFF), level 0–200 (0.5 % steps), command, duration |
| `0x1FEDA` | DC_DIMMER_STATUS_3  | RX | instance, operating level, load status (derived from level > 0) |
| `0x1FFB7` | TANK_STATUS | RX | broadcast by the Garnet SeeLevel II 709-RVC (source addr `0x48`, 3 sensors on this coach); instance, relative level, resolution, absolute level (L), tank size (L) — see *Tank sensors* below |

29-bit ID: priority (default 6) bits 26–28, DGN bits 8–24, source address
bits 0–7. Commands used: 0 set-level, 2 on, 3 off, 4 stop, 5 toggle,
**19/20** ramp up/down (full enum in `rvc_protocol.h`). ⚠️ The enum was
misnumbered before 2026-08-08 (17/18 as ramp up/down — actually "ramp
brightness"/"ramp toggle"); spamming 17 during hold-to-dim wedged the G6A's
dimmer engine for that load until the G6 was power-cycled. Command codes are
now cross-checked against rvc-proxy and rvc2hass (both proven on real
coaches) — don't renumber without a captured factory frame. Byte 5
(interlock) must be **0x00**, never 0xFF. ON/OFF taps carry an explicit
level of 200 (100 %), matching the proven frame
`[inst FF C8 cmd FF 00 FF FF]`.

Dimmer button behavior: tap = toggle (multi-instance buttons send explicit
ON/OFF to all members so they can't desync; shown ON if **any** member is
on), press-and-hold = ramp (direction alternates per hold, re-sent while
held, STOP on release).

### Tank sensors (SeeLevel II 709-RVC) — displayable, screen-2 widget on MID COACH

This coach has a Garnet SeeLevel II 709-RVC tank monitor with 3 sensors
(Fresh, Grey, Black) on the RV-C bus. It broadcasts read-only
`TANK_STATUS` (`0x1FFB7`) — a **different DGN from the dimmer loads
above**, decoded on its own path (`twai_tasks.c` → a dedicated tank state
table in `state_manager.c`, never the dimmer instance table) and displayed
via the read-only `PANEL_BTN_TANK_LEVEL` button type.

**Byte layout and instance mapping bus-confirmed 2026-08-15** via sniffer
mode on `living_room`: instance 0 = fresh, 1 = black, 2 = gray (matches
the public research). **Percent formula corrected against real captured
frames** — public sources (Garnet/Victron docs,
`linuxkidd/coachproxy-os`'s `rvc-spec.yml`) describe it as
`relative_level / resolution`, but that's wrong for this unit: resolution
is the SeeLevel's total capacitive-sensor-segment count for that tank's
strip (varies per instance — 32 vs 28 on this coach), not a
percent-per-count divisor, so plain integer division always truncated to
0. Correct formula: `percent = relative_level * 100 / resolution`. Full
detail and the three captured frames (also used as
`host_test/test_rvc.c` regression vectors) are in
[docs/instance_map.yaml](docs/instance_map.yaml) → `tank_dgn`.

Live on `panels/living_room.h` ("MID COACH") as of GitHub issues #4/#5:
screen 2 shows FRESH/GREY/BLACK with a BACK button, reached via a
**TANK LEVELS** button in screen 1's bottom-right slot (see *Dual screens*
below). Not yet wired to `ent_center` or `living_room_remote` — the latter
has no CAN wiring and can't see `TANK_STATUS` frames directly (ESP-NOW
relay of tank data is explicitly out of scope for now, see
`docs/SPEC-panel-v2.md`).

**Known limitation, accepted 2026-08-15: displayed percent won't exactly
match the SeeLevel unit's own front-panel digits.** Compared directly on
the bench: Fresh panel 12% vs SeeLevel 9%, Grey 67% vs 68% (near-exact),
Black 10% vs 14%. Likely cause: the SeeLevel's own display applies
per-tank shape compensation (correcting the raw "N of M segments wet"
ratio for non-rectangular tank geometry) that isn't exposed over RV-C —
`TANK_STATUS` appears to carry the raw, uncompensated ratio. Reverse-
engineering that curve isn't practical from the bus alone. See
[docs/instance_map.yaml](docs/instance_map.yaml) → `tank_sensors` for the
full comparison and reasoning.

## Instance map (from factory Entegra legends — verify via sniffer!)

| Load | Instances | Panels |
|------|-----------|--------|
| CENTER CEILING  | 25 | both |
| ACCENT          | 26, 27 | both |
| SIDE CEILING    | 30, 31 | both |
| ODS SOFA SCONCE / ODS SLIDE | 32 | both |
| DINETTE / SCONCE-DINETTE    | 33 | both |
| SINK/COUNTER    | 34 | both |
| MIDSHIP / HALL-MIDSHIP      | 35 | both |

`living_room` (on-screen "MID COACH") and `ent_center` were realigned
2026-08-15 to drive the same instance set. **ENTRY CEILING (24)** and
**SECURITY P+H (44, 45 — Note A)** are no longer wired to any CAN-connected
panel (they were dropped from `living_room.h`'s button grid); both remain
valid RV-C instances, just not currently exposed by a button. See
[docs/instance_map.yaml](docs/instance_map.yaml) for the full instance map
including everything not yet wired to firmware.

- **Note A:** factory legend "P45, H44, 45". Was implemented as switch
  (on/off) on instances 44+45; the P/H prefixes may indicate a different
  load type or a scene. Verify via sniffer before trusting if re-added.
- **Note B:** PL1 is the factory switch-panel backlight — probably not a
  DC_DIMMER instance. Never implemented as a real DGN (the old manual
  PANEL LIGHTS button only ever drove the panel's own LCD locally, never a
  CAN frame — see the *Automatic backlight* note in the UI section, which
  replaced that button with automatic idle dim/off). Capture factory
  frames in sniffer mode to implement the real DGN if PL1 is ever wanted.
  Lead: on Spyder-based coaches panel backlights use
  `GENERIC_INDICATOR_COMMAND` (status `0x1FED7`) with group = panel ID and
  function 0 = set brightness (per CoachProxy's Tiffin notes) — Firefly may
  do the same.

## Board pinout (Waveshare 4.3B)

I2C: SDA 8 / SCL 9 (GT911 @0x5D + CH422G). Touch INT: GPIO4. RGB: DE 5,
VSYNC 3, HSYNC 46, PCLK 7, data B3–B7/G2–G7/R3–R7 =
14,38,18,17,10 / 39,0,45,48,47,21 / 1,2,42,41,40. CH422G EXIO: 1 = TP_RST,
2 = backlight enable (on/off only), 3 = LCD_RST.

### Open pin/protocol TODOs (all marked in code)

1. **TWAI TX/RX = GPIO15/16, empirically confirmed correct** — the
   2026-08-08/09 coach test had commands reaching the G6 and toggling a real
   load, which only works if these pins drive the actual TJA1051 transceiver
   (`board_4_3b.h`). Still worth a formal schematic diff at some point, but
   this is no longer a "might be wrong" open item in practice.
2. Source address `0x80 + PANEL_INDEX` — sniff the bus for collisions before
   deploying (`main/panel_config.h`).
3. `DC_DIMMER_COMMAND_2` byte 1 (group) sent as 0xFF — compare against
   captured factory switch frames (`rvc_protocol.c`). Byte 5 (interlock) is
   resolved: must be 0x00 (0xFF suspected in the 2026-08-08 G6 load-latchup;
   all proven implementations send 0x00).
4. `DC_DIMMER_STATUS_3` bytes 3–6 semantics unverified across RV-C revisions
   (`rvc_protocol.c`); `load_on` is derived from level > 0.
5. GT911 may sit at 0x14 instead of 0x5D depending on INT strap at reset
   (`board_4_3b.c`).
6. Backlight is on/off only via CH422G; dimming is emulated with an LVGL
   overlay. Probe for a PWM-capable backlight route (`board_4_3b.h`, `ui.c`).
7. RGB timings copied from the Waveshare 4.3 demo — diff against the current
   4.3B demo release at hardware bring-up (`board_4_3b.c`).

## UI

Dark night theme (near-black screen bg) in `ui_theme.h`. Status bar (36 px):
panel name, left. The CAN-health dot that used to sit on the right (green/
red circle) was removed 2026-08-15 (GitHub issue #8) — no per-panel toggle
existed for it, so it's gone from every panel's header, not just
`living_room`'s. On a panel with `PANEL_HAS_SCREEN_2` and GREY/BLACK tank
buttons (today: only `living_room`), that space now shows the Grey/Black
tank status readout described below (issue #9); other panels' headers are
just the name.

**Grey/Black tank status readout + critical backlight override (GitHub
issue #9, `living_room`/"MID COACH" only).** `build_screen()` in
`main/ui/ui.c` finds the screen 2 buttons labeled `"GREY"`/`"BLACK"` by
scanning `PANEL_BUTTONS_2` (no hardcoded instance numbers in `ui.c`) and, if
both exist, creates a status-bar label updated every 500 ms by
`tank_status_timer_cb`, which polls `state_manager_get_tank()` for both:
"Grey-Black OK" (white, `UI_COLOR_TEXT`) below 80 %, "Grey-Black Warn"
(orange, `UI_COLOR_WARN`) at 80–88 %, "Grey-Black FULL" (red,
`UI_COLOR_ERR`, blinking every tick) at 89 %+. While FULL, a static
`s_tank_critical` flag forces `idle_timer_cb` to hold the backlight at
100 % and skip the normal 120 s/300 s dim/off stages entirely — this is a
"go empty the tank" alert, not something that should ever dim out of view.

**Automatic backlight (GitHub issue #3, replaces the old manual PANEL
LIGHTS button):** `idle_timer_cb` in `main/ui/ui.c` tracks
`lv_display_get_inactive_time()` through a 3-state
`backlight_state_t` (`BACKLIGHT_NORMAL` / `BACKLIGHT_DIMMED` /
`BACKLIGHT_OFF`) — 120 s idle dims to 20 %, 300 s idle turns the backlight
fully off via `board_backlight_set_percent(0)` (a real CH422G EXIO2
disable, not just a fully-opaque overlay). Either stage wakes to 100 % on
the first touch, which is absorbed by the top-layer dim overlay and never
reaches a button underneath. There is no manual brightness button anymore
— see `docs/SPEC-panel-v2.md` for the design and open risks (notably:
does GT911 touch still register with the backlight physically off —
confirm on the bench before trusting the OFF stage on a coach-installed
panel).

**Buttons (2026-08-15, coach-installed and confirmed working with the new
color scheme on `living_room`/"MID COACH"):** label text only, no icon glyph — the
`panel_btn_def_t.symbol` field was removed along with it, so `panels/*.h`
button rows no longer take an `LV_SYMBOL_*` argument (see any panel header
for the current field order). Label font bumped from Montserrat 16 to 20.
Background is the on/off indicator: dark blue (`UI_COLOR_CARD`, off) to
light blue (`UI_COLOR_CARD_ON`, on), set on the button object itself in
`ui_dimmer_button.c`'s `refresh_visuals()`. Name-label text color still
flips (`UI_COLOR_TEXT_DIM` off, `UI_COLOR_TEXT_ON_LIT` — dark navy — on) for
contrast against the light-blue on-state. The dimmer level bar keeps its own
amber (`UI_COLOR_AMBER`) fill, independent of this background swap.

**Dual screens (GitHub issue #4).** A panel opts in with
`#define PANEL_HAS_SCREEN_2 1` (default 0, `main/panel_config.h`) plus a
second `PANEL_BUTTONS_2[]`/`PANEL_BUTTON_COUNT_2` array. `build_screen()`
in `main/ui/ui.c` builds both screens up front as sibling containers
(screen 2 starts `LV_OBJ_FLAG_HIDDEN`) so status updates keep both correct
even while one is hidden — switching back must never show stale state. One
`PANEL_BTN_SCREEN_SWITCH` button per screen calls `switch_screen()` to
toggle which one is visible; it's local UI nav only, never forwarded as an
RV-C command.

Screen 1 stays the plain 2x4 button grid (`build_button_grid()`);
`PANEL_BTN_SPACER` fills a grid cell with nothing, which is how it
positions a button at a specific cell instead of wherever sequential fill
would put it. Screen 2 is assumed to be a tank readout — the only panel
that defines one is `living_room` — and since GitHub issue #10/#11
(2026-08-15) uses its own layout builder, `build_screen2_tanks()`: its
`PANEL_BTN_TANK_LEVEL` entries lay out as a centered horizontal row of
`ui_tank_wave` gauges (`components/ui_common/ui_tank_wave.c`, see below),
and its one `PANEL_BTN_SCREEN_SWITCH` entry ("BACK") is a small button
pinned to the bottom center rather than an equal grid cell — see
`panels/living_room.h`'s `PANEL_BUTTONS_2[]` (just the 3 tank buttons +
BACK now; no manual spacer positioning needed for this layout). If a
future panel wants a non-tank screen 2, `build_screen2_tanks()` will need
to stop assuming that.

**Wave-style tank gauge (GitHub issue #10).**
`components/ui_common/ui_tank_wave.c` replaces the old plain progress-bar
tank widget: a rounded "glass" container (`UI_COLOR_TANK_EMPTY` background,
clipped to its rounded corners via `lv_obj_set_style_clip_corner`) holding
a water-fill object and a percent label pinned to the top. The fill's
`LV_EVENT_DRAW_MAIN` handler draws a flat rect up to the fill line plus a
short sine-wave polyline riding that line (`lv_draw_line` with a
multi-point array), phase-advanced ~every 100 ms by a per-widget
`lv_timer` calling `lv_obj_invalidate()` — reads as moving water without
needing true polygon fill. All three tanks share one water color
(`UI_COLOR_CARD_ON`); no per-tank hue coding was requested.
`ui_dimmer_button.c`'s `PANEL_BTN_TANK_LEVEL` branch creates this widget
instead of a bar/label pair; `ui_dimmer_button_update_tank()` just calls
`ui_tank_wave_set_percent()`.

**Display orientation: portrait (90° CW).** `lvgl_init()` in `board_4_3b.c`
sets `.sw_rotate = true` and calls `lv_display_set_rotation(disp,
LV_DISPLAY_ROTATION_90)`; LVGL applies the inverse transform to GT911 touch
coordinates automatically, no touch-driver flags needed. The button grid
sizes itself off `lv_display_get_vertical_resolution()` (logical, i.e.
post-rotation) rather than `BOARD_LCD_V_RES` so it fills correctly regardless
of rotation — see `main/ui/ui.c` `build_screen()`. **Do not set
`.flags.full_refresh` together with `sw_rotate`**: in this esp_lvgl_port
version, `full_refresh` (like `direct_mode`) waits on `disp_ctx->trans_sem`,
which is only allocated when `avoid_tearing = true` — and `avoid_tearing`
can't be combined with `sw_rotate` on RGB panels (it hands LVGL the physical
PSRAM framebuffers, leaving no room for rotation). That combination asserts
on the very first flush (`assert failed: xQueueSemaphoreTake`, right after
"display up" in the boot log) — hit and fixed during the portrait-mode
bring-up; verified stable on hardware without it. Partial-render mode (the
default when both flags are unset) doesn't touch `trans_sem`, and `bb_mode`
already covers tear-free RGB output without `avoid_tearing`'s scheme.

**Tap-to-toggle command confirmation.** RV-C has no command-ack DGN, so a
tap sends the dimmer command once, then arms an ~800 ms LVGL timer
(`ui_dimmer_button.c`) waiting for the `DC_DIMMER_STATUS_3` that reflects the
new state; if it doesn't arrive in time the command is resent once (bounded
retry). **The button's visual state is still driven only by real status
frames** — the confirm timer never touches `ctx->on[]`/`ctx->levels[]`
itself, it only decides whether to resend. Don't reintroduce an optimistic
local flip on tap; that was tried and reverted because it breaks the
status-driven-UI invariant above (a panel could show a state that disagrees
with the real load if the resend also gets lost). Hold-to-dim uses continuous
`RAMP_UP`/`RAMP_DOWN` while held (not a stepped percentage cycle) — this
matches the standard RV-C dimmer pattern and how Firefly's own switches
behave; `LV_OBJ_FLAG_PRESS_LOCK` must stay enabled (LVGL's default — don't
remove it) or small finger drift cancels the long-press before it fires.

## Panels & source-address allocation

`panels/REGISTRY.md` is the **canonical allocation record** for `PANEL_INDEX`.
Source address = `0x80 + PANEL_INDEX` (`main/panel_config.h`); duplicates put
two nodes at the same CAN address and produce intermittent frame loss rather
than an obvious failure.

**Always resolve this before touching `panels/`:**

- **A — updating an existing panel** (buttons, labels, instances): edit that
  panel's header, **keep its `PANEL_INDEX`**, no registry change.
- **B — adding a new panel**: take *Next free index* from `REGISTRY.md`, copy
  `panels/TEMPLATE.h`, add a registry row, bump *Next free index*.

Validate either way with `python tools/check_panels.py` — it fails on
duplicate indices, headers missing from the registry, stale registry rows, and
a wrong *Next free index*. CI runs it on every push and derives its build
matrix from it, so a new panel gets build coverage automatically.

`panels/TEMPLATE.h` carries an `#error` guard so it can never be flashed as-is.

## OTA / Wi-Fi update path

Panels are installed **inside walls**, so `partitions.csv` reserves a dual-OTA
layout on the 16 MB flash: `ota_0` / `ota_1` at 4 MB each (current app ~0.65 MB),
8 KB `otadata`, and ~8 MB `storage` for future icon fonts, captured bus logs,
and config.

Rollback is on (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). An OTA image boots
as `ESP_OTA_IMG_PENDING_VERIFY`; `app_main()` calls
`esp_ota_mark_app_valid_cancel_rollback()` **only after** display, touch, UI,
and the RV-C tasks are up. A bad update reverts itself on the next reset
instead of requiring a panel to come out of the wall. Do not move that call
earlier — it is the health gate.

**Not yet implemented:** Wi-Fi bring-up and the update transport. Remaining
work is (1) station/provisioning config, (2) `esp_https_ota()` pull or a local
push endpoint, (3) a way to target one panel or all. None of it requires
repartitioning — that is the entire point of fixing the layout now, before
panels are sealed in. Changing `partitions.csv` after installation invalidates
flash and forces USB reflashing of every panel.

## Conventions / decisions

- ESP-IDF style C; 4-space indent; `s_` prefix for file-static state.
- Protocol logic stays ESP-free in `rvc_protocol` so it remains host-testable.
- New status DGNs get decoded in `twai_rx_task` and flow through the state
  manager — widgets never parse frames.
- Adding a panel = one header in `panels/` + a build flag. No C changes.
  Resolve the A/B question above first; procedure in
  [docs/FLASHING.md](docs/FLASHING.md#adding-a-new-panel).
- Licensed under The Unlicense (public domain). Don't add code that can't be
  released that way — no vendored GPL sources, no proprietary vendor blobs.
- Decision log: this file, section above. Bench findings (verified pins,
  captured DGNs, instance corrections) should update the tables here and the
  matching TODO comments in code.
- **Development workflow (as of 2026-08-15):** multi-file features get a
  GitHub issue first (`enhancement` label for new capability, `bug` for a
  defect), scoped to one reviewable piece of work — see the open issues on
  [docs/SPEC-panel-v2.md](docs/SPEC-panel-v2.md) for the current example
  (one issue per lettered section: A/B/C). Implementation happens on a
  branch, gets tested (host tests + simulator, bench where applicable),
  then goes up as a PR that references/closes its issue and runs CI before
  merge — not a direct push to `main`. Small stuff (docs, instance-map
  corrections, single-panel button tweaks) still goes straight to `main`
  as before; this applies to the kind of change that needs its own spec
  section per this file's spec-discipline rule.

## Repo hygiene — what belongs in git

Committed: C/H sources, `CMakeLists.txt`, `idf_component.yml`,
`dependencies.lock` (pins component versions — do **not** delete it),
`sdkconfig.defaults`, `partitions.csv`, `Kconfig.projbuild`, `panels/*.h`,
`panels/REGISTRY.md`, `tools/`, `.github/workflows/`, `.gitattributes`,
`LICENSE`, `sim/` **source**, docs and `docs/images/*.png`. The only permitted
IDE file is `.vscode/extensions.json` (recommends the ESP-IDF extension).

Never committed: `build*/`, `managed_components/`, `sdkconfig` (generated from
defaults), `sim/third_party/` (SDL2), `sim/build*/`, any compiled artifact
(`*.exe/.o/.a/.bin/.elf`), IDE folders, toolchains. The `.gitignore` covers
these; verify a change with `git add -A --dry-run` before committing.

Machine-specific notes go in `CLAUDE.local.md` (gitignored), never in this
file.

## Development notes & gotchas

Hard-won during setup — check here before re-debugging:

- **PowerShell does not expand `$var` in arguments starting with `-`.**
  `cmake -DPANEL=$Panel` passes the literal string `$Panel`. Quote the whole
  argument: `cmake "-DPANEL=$Panel"`. This silently produced a "panel not
  found" error deep in a rebuild, not at configure time.
- **`. C:\esp\esp-idf\export.ps1` is per-shell.** Any script that calls
  `idf.py`, `cmake`, or `ninja` must source it first (or check
  `Get-Command cmake` and source on demand, as `sim/build.ps1` does).
- **Suppressing script output hides real failures.** Piping cmake through
  `Select-Object -Last N` swallowed a configure error and surfaced it later as
  a confusing ninja failure. Check `$LASTEXITCODE` after each step.
- **The GT911 config macro is `ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()`** — with
  `I2C` in the name. `ESP_LCD_TOUCH_IO_GT911_CONFIG()` does not exist and
  fails as "invalid initializer". Backup address constant is
  `ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP` (0x14).
- **LVGL's default 64 KB internal heap is too small for `lv_snapshot_take()`**
  on an 800×480 screen. The simulator's `lv_conf.h` sets
  `LV_USE_STDLIB_MALLOC = LV_STDLIB_CLIB`; the firmware keeps LVGL's allocator.
- **The simulator needs `managed_components/` populated**, so run one
  `idf.py build` on a fresh clone before building `sim/`.
- Long ESP-IDF operations (clone, `install.ps1`, first build) take many
  minutes — run them in the background rather than blocking on a timeout.

Hard-won during first hardware bring-up (2026-08-05, bench board = plain
ESP32-S3-Touch-LCD-4.3, **not** the B):

- **`direct_mode = true` is mandatory with `avoid_tearing`** in the
  esp_lvgl_port display config (`board_4_3b.c`). Without it LVGL runs in
  partial mode and each swap can present a framebuffer holding only the last
  dirty region — on hardware: UI alternating with a full white frame at the
  cadence of the 500 ms CAN-health timer, garbling on touch. The steady blink
  *rate matching an LVGL timer* is the signature.
- **Never use the RESET button while a serial port is open** — the chip lands
  in ROM download mode (`boot:0x0 DOWNLOAD`) every time, both DTR polarities,
  app running or idle. Recovery/boot = power cycle (which has worked every
  time) or esptool's own reset during a flash. Manual download-mode entry:
  hold BOOT, tap RESET, release BOOT.
- **Undersized 5 V supply mimics firmware bugs.** The plain 4.3 is spec'd
  5 V / 450 mA via USB-C only. On a weak port/dock: dim backlight, white
  flashes under load, USB "device descriptor request failed", chip that won't
  enumerate. Bench with a 2 A wall charger in the USB port + UART cable
  (direct to PC, not a dock) for logs/flash.
- **The plain 4.3 variant has no wide DC input.** Its `- +` HY2.0 connector is
  the 3.7 V lithium **battery** header (CS8501 charger) — 12 V there destroys
  the board. Only the 4.3B takes 7–36 V. Check the variant before wiring
  bench power.
- **Console capture:** app logs go to UART0 (the `UART` USB-C port, CH343).
  The `USB` port (native USB-Serial-JTAG) drops and re-enumerates on every
  reboot and misses early boot — prefer the UART port for bring-up logging.
- **SpotPear docs for the plain 4.3 map CAN to GPIO 19/20 and RS-485 to
  GPIO 15/16** — i.e. our unverified `BOARD_TWAI_TX/RX = 15/16` would drive
  the RS-485 transceiver on that variant, and CAN on 19/20 conflicts with
  native USB. Still unverified against a schematic for either variant; the
  `TODO(critical)` in `board_4_3b.h` stands.

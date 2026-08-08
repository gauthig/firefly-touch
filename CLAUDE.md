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
download if missing). Note: in `--shot` mode the CAN dot is red because the
500 ms health timer never fires in the single rendered frame; interactively
it goes green.

## Architecture

One codebase, many panels: `panels/<name>.h` (selected at build time via
`-DPANEL=`) defines `PANEL_NAME`, `PANEL_INDEX`, and the `PANEL_BUTTONS[]`
grid layout. RV-C source address = `0x80 + PANEL_INDEX`.

### Task map

| Task           | Core | Prio | Role |
|----------------|------|------|------|
| `twai_rx`      | 0    | 12   | blocks on `twai_receive`, stamps bus liveness, sniffer logging, decodes `DC_DIMMER_STATUS_3`, posts to status queue |
| `twai_tx`      | 0    | 11   | drains TX queue → `twai_transmit`; bus-off recovery. Nothing else ever blocks on the bus |
| `state_mgr`    | 0    | 9    | owns instance→{level,on} table; on change calls `ui_on_status()` under the LVGL lock |
| LVGL (port)    | 1    | 4    | rendering + touch; button callbacks only enqueue to the TX queue |

**Invariant:** icon/button visual state is driven ONLY by status frames from
the bus — never by locally sent commands. That keeps panels in sync with the
factory switches and the Firefly app.

Data flow: touch → `ui_dimmer_button` event → `panel_send_cb` →
`twai_enqueue_dimmer_cmd()` → TX queue → `twai_tx` → bus …then… bus →
`twai_rx` → status queue → `state_mgr` → `ui_on_status()` → widget.

### Components

- `components/rvc_protocol` — pure C (no ESP deps): 29-bit ID pack/unpack,
  DGN encode/decode. Host-testable (`host_test/`).
- `components/board_4_3b` — Waveshare 4.3B bring-up: RGB timings, CH422G
  (custom minimal driver, `ch422g.c`), GT911, esp_lvgl_port glue, TWAI init.
  RGB timing / pin values follow Waveshare's published demo — **merge points
  are marked in `board_4_3b.c`; diff against the current Waveshare demo when
  bringing up hardware, don't invent timings.**
- `components/ui_common` — theme (`ui_theme`), shared dimmer-button widget
  (`ui_dimmer_button`), panel-config types (`panel_def.h`). Icons are LVGL
  built-in symbols for now; `.symbol` is an opaque string so a custom icon
  font can be swapped in later.

## RV-C protocol

| DGN | Name | Direction | Use |
|-----|------|-----------|-----|
| `0x1FEDB` | DC_DIMMER_COMMAND_2 | TX | instance, group(0xFF), level 0–200 (0.5 % steps), command, duration |
| `0x1FEDA` | DC_DIMMER_STATUS_3  | RX | instance, operating level, load status (derived from level > 0) |

29-bit ID: priority (default 6) bits 26–28, DGN bits 8–24, source address
bits 0–7. Commands used: 0 set-level, 2 on, 3 off, 4 stop, 5 toggle,
17/18 ramp up/down (full enum in `rvc_protocol.h`).

Dimmer button behavior: tap = toggle (multi-instance buttons send explicit
ON/OFF to all members so they can't desync; shown ON if **any** member is
on), press-and-hold = ramp (direction alternates per hold, re-sent while
held, STOP on release).

## Instance map (from factory Entegra legends — verify via sniffer!)

| Load | Instances | Panels |
|------|-----------|--------|
| ENTRY CEILING   | 24 | living_room |
| CENTER CEILING  | 25 | both |
| ACCENT          | 26, 27 | ent_center |
| SIDE CEILING    | 30, 31 | both |
| ODS SOFA SCONCE / ODS SLIDE | 32 | both |
| DINETTE / SCONCE-DINETTE    | 33 | both |
| SINK/COUNTER    | 34 | ent_center |
| MIDSHIP / HALL-MIDSHIP      | 35 | both |
| SECURITY P+H (patio+hitch)  | 44, 45 | living_room — **unverified, see Note A** |
| PANEL LIGHTS (PL1)          | n/a | both — **placeholder, see Note B** |

- **Note A:** factory legend "P45, H44, 45". Implemented as switch (on/off)
  on instances 44+45; the P/H prefixes may indicate a different load type or
  a scene. Verify via sniffer before trusting.
- **Note B:** PL1 is the factory switch-panel backlight — probably not a
  DC_DIMMER instance. Currently logs the press and cycles the local LCD
  brightness (100→60→20 %). Capture factory frames in sniffer mode to
  implement the real DGN.

## Board pinout (Waveshare 4.3B)

I2C: SDA 8 / SCL 9 (GT911 @0x5D + CH422G). Touch INT: GPIO4. RGB: DE 5,
VSYNC 3, HSYNC 46, PCLK 7, data B3–B7/G2–G7/R3–R7 =
14,38,18,17,10 / 39,0,45,48,47,21 / 1,2,42,41,40. CH422G EXIO: 1 = TP_RST,
2 = backlight enable (on/off only), 3 = LCD_RST.

### Open pin/protocol TODOs (all marked in code)

1. **TWAI TX/RX = GPIO15/16 is UNVERIFIED against the 4.3B schematic —
   verify before first flash** (`board_4_3b.h`). Wrong pins can disturb the
   live coach bus.
2. Source address `0x80 + PANEL_INDEX` — sniff the bus for collisions before
   deploying (`main/panel_config.h`).
3. `DC_DIMMER_COMMAND_2` byte 1 (group) and byte 5 (interlock) sent as 0xFF —
   compare against captured factory switch frames (`rvc_protocol.c`).
4. `DC_DIMMER_STATUS_3` bytes 3–6 semantics unverified across RV-C revisions
   (`rvc_protocol.c`); `load_on` is derived from level > 0.
5. GT911 may sit at 0x14 instead of 0x5D depending on INT strap at reset
   (`board_4_3b.c`).
6. Backlight is on/off only via CH422G; dimming is emulated with an LVGL
   overlay. Probe for a PWM-capable backlight route (`board_4_3b.h`, `ui.c`).
7. RGB timings copied from the Waveshare 4.3 demo — diff against the current
   4.3B demo release at hardware bring-up (`board_4_3b.c`).

## UI

Dark night theme (near-black bg, warm amber accents) in `ui_theme.h`. Status
bar (36 px): panel name + CAN-health dot (green if any bus frame in the last
5 s, else red). Backlight auto-dims to 20 % after 5 min idle (300 s); the
waking touch is absorbed by the top-layer overlay and never reaches a button.
`idle_timer_cb` logs `inactive_ms` every second while the idle-dim path is
under verification — remove once confirmed on 4.3B.

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

# firefly-touch

Replacement touchscreen wall panels for a 2019 Entegra Aspire 44W with a
Firefly Integrations G6A multiplex system. Each panel is an **independent peer
node** on the coach's RV-C bus (CAN 2.0B, 250 kbps, 29-bit extended IDs).
There is no hub or gateway — the bus is the shared state mechanism.

Hardware: Waveshare **ESP32-S3-Touch-LCD-4.3B** (ESP32-S3-WROOM-1-N16R8,
16 MB flash / 8 MB octal PSRAM, 4.3" 800x480 RGB LCD, GT911 touch on I2C,
CH422G IO expander, TJA1051 CAN transceiver, 7–36 V input).

## Build / flash / monitor

Requires ESP-IDF **v5.3+** (component manager pulls `lvgl/lvgl ^9`,
`espressif/esp_lvgl_port ^2`, `espressif/esp_lcd_touch_gt911 ^1`).

```
idf.py -B build_living_room -DPANEL=living_room build
idf.py -B build_ent_center  -DPANEL=ent_center  build
idf.py -B build_living_room -DPANEL=living_room flash monitor
```

Use one build dir per panel (PANEL is cached; switching values in a shared
build dir requires `fullclean`). Valid PANEL values = basenames of headers in
`panels/`.

Sniffer mode (log every RV-C frame — how the instance map gets verified):
`idf.py menuconfig` → *Firefly Touch Panel* → *RV-C sniffer mode*, or add
`CONFIG_FIREFLY_SNIFFER_MODE=y` to `sdkconfig.defaults` for a bench build.

Host unit tests (pure-C protocol code, any host compiler):

```
cd components/rvc_protocol/host_test
gcc -Wall -Wextra -I../include ../rvc_protocol.c test_rvc.c -o test_rvc && ./test_rvc
```

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
5 s, else red). Grid cells ≈ 397×108 px. Backlight auto-dims to 20 % after
60 s idle; the waking touch is absorbed by the top-layer overlay and never
reaches a button.

## Conventions / decisions

- ESP-IDF style C; 4-space indent; `s_` prefix for file-static state.
- Protocol logic stays ESP-free in `rvc_protocol` so it remains host-testable.
- New status DGNs get decoded in `twai_rx_task` and flow through the state
  manager — widgets never parse frames.
- Decision log: this file, section above. Bench findings (verified pins,
  captured DGNs, instance corrections) should update the tables here and the
  matching TODO comments in code.

# Custom RV-C DGN family — climate, batteries, shore power, solar

**Status: DESIGN, awaiting validation. Nothing here is built.** Follows the bus survey and probe
(issue #96, [BUS-SURVEY.md](BUS-SURVEY.md)), which showed this coach's bus is nearly idle and that
the Firefly G6 ignores unknown DGNs in the block below.

## 1. Direction and scope

The RV-C bus becomes the source of truth for everything an RV-C panel can reach. ESP-NOW stays only
for panels with no CAN wiring (today `bedroom_remote`) and for the dump valves. These DGNs are
**our own, for this coach**: they are not added to the G6's world, follow the RV-C *conventions*
(8-byte frames, instance in byte 0, 0xFF for not-available) but not its message catalogue.

Non-goals: no address claiming (none seen on this bus), no change to the dimmer/tank DGNs, no valve
traffic, no encryption (the bus is wired; see issue #95 for the wireless leftovers).

## 2. The block

**`0x0FFC0`–`0x0FFCF`** (16 DGNs). J1939 proprietary-B space: PDU2, data page 0, PF `0xFF`.

| evidence | status |
|---|---|
| Never seen on the live bus (15 min survey) | all 16 |
| Transmitted for 12 min at realistic rates, G6 and others unchanged | `0FFC0`–`0FFC7` (the probe sends offsets 0–7) |
| Checked free on the bus but **never transmitted** | `0FFC8`–`0FFCF` (pack detail, bank B, command, solar B, pedestal, spares): extend the probe to send all 16 and repeat stage 2 before first use |

Priority 6 (as the existing panels), DLC always 8.

## 3. Conventions

- **Little-endian** multi-byte fields.
- **Byte 0 is the instance** (zone / pack / line / controller / bank), so the survey tool keys each
  stream separately.
- **Not available:** unsigned = all ones (`0xFF`, `0xFFFF`); signed = `0x7F` / `0x7FFF`.
- **Reserved bytes are `0xFF`.**
- **Temperature, compact:** `°C = raw × 0.5 − 40` (u8, −40 … +87.5 °C, `0xFF` n/a). Solar controller
  temperatures: `°F = raw − 40` (u8, −40 … +214 °F, `0xFF` n/a).
- **Freshness.** A producer sends periodically even when nothing changed. A consumer keeps feeding the
  same `ui_on_*()` entry points the ESP-NOW telemetry uses today, so the existing per-widget staleness
  rules (shore 20 s, batteries 3× poll, HVAC 3×) are unchanged. Silence means the producer is gone; a
  producer that is alive but cannot reach its source sends `online = 0` instead.
- **No repeats or de-duplication.** CAN frames are acknowledged and retransmitted by hardware, which
  is what the ESP-NOW broadcast design had to emulate in software.

## 4. Layouts

### 0FFC0 — HVAC zone status (producer `hvac_panel`, 2 s per zone)

| byte | field |
|---|---|
| 0 | zone, 0..3 |
| 1 | flags: bit0 zone present, bit1 data fresh (last BLE read < 90 s old) |
| 2 | mode, EasyTouch native: 0 off, 1 fan, 2 cool, **4 Aqua-Hot**, **7 electric heat**, 11 auto |
| 3 | current mode: 0 idle, 2 cooling, 4 heating (either source) |
| 4 | fan for the selected mode (EasyTouch fan enum: 0 off, 1 low, 2 high, 65/66 cycled, 128 auto) |
| 5 | inside temperature, °F, s8 (`0x7F` n/a) |
| 6 | cool setpoint, °F, u8 |
| 7 | heat setpoint, °F, u8 |

Using the EasyTouch mode numbers directly keeps the Aqua-Hot / electric distinction that the standard
RV-C thermostat message cannot express. Heat Pump versus Heat Strip stays one value (7), as today.

### 0FFC1 — HVAC zone extras (`hvac_panel`, 10 s per zone)

| byte | field |
|---|---|
| 0 | zone |
| 1 | auto heat setpoint °F |
| 2 | auto cool setpoint °F |
| 3 | age of the last BLE read, seconds (saturates at 254; `0xFF` n/a) |
| 4 | fan-only level (0/1/2) |
| 5 | cool fan |
| 6 | heat fan |
| 7 | auto fan |

### 0FFCA — HVAC command (any panel → `hvac_panel`, event driven)

| byte | field |
|---|---|
| 0 | zone |
| 1 | op: 0 mode, 1 cool setpoint, 2 heat setpoint, 3 auto heat setpoint, 4 auto cool setpoint, 5 cool fan, 6 heat fan, 7 auto fan, 8 fan-only |
| 2 | argument (u8) |
| 3–7 | reserved |

`hvac_panel` applies it through the existing `easytouch_client_submit_change()`. The next status is the
confirmation, as today; panel UIs keep their optimistic-setpoint behaviour.

### 0FFC2 / 0FFC3 / 0FFC4 — battery pack 1 / 2 / 3 (producer proxy, 5 s each)

One DGN per pack. Byte 0 repeats the pack number (1..3).

| byte | field |
|---|---|
| 0 | pack 1..3 |
| 1 | flags: bit0 online, bit1 temperatures valid |
| 2 | state of charge, % |
| 3–4 | volts, u16, 0.01 V |
| 5–6 | current, s16, 0.1 A, **positive = charging** |
| 7 | reserved |

### 0FFC8 — battery pack detail (proxy, 10 s per pack)

| byte | field |
|---|---|
| 0 | pack 1..3 |
| 1–2 | residual capacity, u16, 0.1 Ah |
| 3–4 | full capacity, u16, 0.1 Ah |
| 5 | lowest temperature, compact °C |
| 6 | highest temperature, compact °C |
| 7 | reserved |

The per-pack DGNs plus this one carry everything the pack-detail popup and `jbd_bms_combine()` need.

### 0FFC5 / 0FFC9 — battery bank aggregate (proxy, 5 s each)

Computed on the proxy with the **same shared pure-C `jbd_bms_combine()`** the panels use, so the
aggregation rules are not forked. Two frames because one does not fit.

`0FFC5` — bank A

| byte | field |
|---|---|
| 0 | bank, 1 |
| 1 | packs: high nibble configured (M), low nibble online (N) |
| 2 | state of charge, % (capacity-weighted) |
| 3–4 | volts, u16, 0.01 V (mean) |
| 5–6 | current, s16, 0.1 A (sum, positive = charging) |
| 7 | reserved |

`0FFC9` — bank B

| byte | field |
|---|---|
| 0 | bank, 1 |
| 1–2 | power, s16, W (sum of per-pack V×I; saturates at ±32766, `0x7FFF` n/a) |
| 3–4 | time remaining, u16, minutes (`0xFFFF` n/a / idle) |
| 5 | bank highest temperature, compact °C |
| 6 | bank lowest temperature, compact °C |
| 7 | state: 0 idle, 1 charging, 2 discharging (same ±0.05 A threshold as the UI caption) |

### 0FFC6 — shore power line (proxy, 2 s per line)

Line A and line B, so the load split is visible. Byte 0 is the line.

| byte | field |
|---|---|
| 0 | line: 0 = A (L1), 1 = B (L2) |
| 1–2 | volts, u16, 0.1 V |
| 3–4 | amps, u16, 0.1 A |
| 5–6 | watts, u16, 1 W (saturates at 65534) |
| 7 | frequency, `Hz = 40.0 + raw × 0.1` (40.0–65.4 Hz, `0xFF` n/a) |

### 0FFCC — shore power pedestal (proxy, 10 s)

| byte | field |
|---|---|
| 0 | 0 |
| 1 | line count: 1 = 30 A, 2 = 50 A (line B is sent only when 2) |
| 2 | Watchdog error code (0 OK, 1–9 = E1–E9, 11/12 = F1/F2) |
| 3–7 | reserved |

### 0FFC7 — solar A / 0FFCB — solar B (proxy, 5 s each)

`0FFC7`

| byte | field |
|---|---|
| 0 | controller, 1 |
| 1 | flags: bit0 online |
| 2 | charge state (Renogy enum: 0 off, 1 activated, 2 MPPT, 3 equalizing, 4 boost, 5 float, 6 current limited) |
| 3 | battery state of charge, % |
| 4–5 | battery volts, u16, 0.01 V |
| 6–7 | PV volts, u16, 0.01 V |

`0FFCB`

| byte | field |
|---|---|
| 0 | controller, 1 |
| 1–2 | PV current, u16, 0.01 A |
| 3–4 | PV watts, u16, 1 W |
| 5 | controller temperature, `°F = raw − 40` |
| 6 | battery temperature, `°F = raw − 40` |
| 7 | reserved |

`0FFCD`–`0FFCF` stay spare.

## 5. Rates and bus load

| stream | frames/s |
|---|---|
| HVAC status 3 zones / 2 s, extras 3 / 10 s | 1.8 |
| Battery packs 3 / 5 s, detail 3 / 10 s, bank A+B 2 / 5 s | 1.3 |
| Shore lines 2 / 2 s, pedestal 1 / 10 s | 1.1 |
| Solar A+B 2 / 5 s | 0.4 |
| **Total** | **about 4.6** |

About 0.3 % of 250 kbps, on top of the measured ~0.5 %. The probe already ran ~10 fps for 12 minutes.

## 6. Source addresses

Panels keep `0x80 + PANEL_INDEX`. The headless proxy joins the bus, so `panels/REGISTRY.md` gains a
*headless CAN nodes* table: **proxy = index 5, `0x85`**. `hvac_panel` keeps `0x84` (already reserved).
Observed on this coach, never to be used: `0x8F` (G6), `0x9C`–`0x9F` (wall switch panels), `0xAF`
(tank sensor). So `PANEL_INDEX` must stay below `0x0F`.

## 7. Who produces and who consumes

| node | role | change |
|---|---|---|
| `hvac_panel` | **produces** 0FFC0/1, **consumes** 0FFCA | gains CAN wiring; **climate-only UI**; WiFi and ESP-NOW removed (frees the RAM that made it the tightest node) |
| proxy | **produces** battery, bank, shore, solar | gains a CAN transceiver; BLE only; WiFi and ESP-NOW removed; keeps its 5 BLE links |
| `mid_coach` | consumes everything, **relays to non-CAN panels** | decodes the DGNs and rebroadcasts them in the existing ESP-NOW telemetry formats (unchanged wire format); relays `ESPNOW_FRAME_HVAC_CMD` onto 0FFCA |
| `main_cabinet` | consumes everything, sends 0FFCA | switches from ESP-NOW telemetry to RV-C; ESP-NOW removed entirely (tanks already arrive on CAN) |
| `bedroom_remote` | unchanged code | its HVAC peer setting points at `mid_coach` instead of `hvac_panel`; telemetry arrives from `mid_coach` |
| valve node | untouched | stays encrypted ESP-NOW to `mid_coach` |

Receivers decode in `twai_rx_task` and hand frames to a queue, then call the same
`ui_on_battery_status()` / `ui_on_shore_power()` / `ui_on_solar_status()` / `ui_on_hvac_status()`
entry points. Widgets never parse frames.

## 8. Implementation shape

- New pure-C component `firefly_dgn`: encode and decode for every layout above with plain scaled-integer
  structs, no ESP or easytouch/jbd dependencies. Host-tested with golden byte vectors.
- Producers and consumers wire it in; a Kconfig kill switch per producer disables publishing.
- The survey/probe build verifies each producer from `mid_coach` (it can hear other nodes, not itself).
- Docs sweep as usual (`SYSTEM.md` diagram, `FLASHING.md`, `CHANGELOG.md`, `REGISTRY.md`).

## 9. Test plan

**Host:** one golden byte vector per DGN; round trip encode→decode; n/a, saturation, negative current,
temperature rounding at the scale boundaries, 30 A single line, pack offline, bank with 2 of 3 packs.
**Simulator:** feed frames into the panel UIs (extend the fake bus) and capture CLIMATE / POWER / BATTERY
screens next to the ESP-NOW-fed originals.
**Bench:** `hvac_panel` on the bus, `mid_coach` survey build confirms each frame at its rate and byte
values; a panel sends 0FFCA and the real thermostat changes; unplug the thermostat link and the fresh flag
clears; proxy at the coach shows 5 BLE links plus CAN with no stalls.
**G6 safety:** survey baseline taken before each producer is enabled and `d` shows 0 deltas with it running;
abort rules as in the probe.
**Regression:** a non-CAN panel behaves identically through the relay; dump valves unaffected.

## 10. Open items

1. Transmit-test `0FFC8`–`0FFCF` (extend the probe to all 16 DGNs, repeat stage 2).
2. Proxy hardware: add a transceiver module to the current board, or replace it with a board that has CAN.
3. Physical drops: stool room (`hvac_panel`) and basement bay (proxy).
4. Issue #95 shrinks to the non-CAN panels and `mid_coach`'s relay; the proxy and `hvac_panel` leave ESP-NOW.

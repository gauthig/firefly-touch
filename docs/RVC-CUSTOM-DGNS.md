# Custom RV-C DGN family — climate, batteries, shore power, solar

**Status: v2.** Architecture approved 2026-11-25 (epic #98). The layouts below are implemented and
host-tested in [`components/firefly_dgn`](../components/firefly_dgn) (issue #100), which is their single
implementation. Producers and consumers are not built yet. Follows the bus survey and probe (issue #96,
[BUS-SURVEY.md](BUS-SURVEY.md)), which showed this coach's bus is nearly idle and that the Firefly G6
ignores unknown DGNs in the block below.

## 1. Direction and scope

The RV-C bus becomes the source of truth for everything an RV-C panel can reach.

- **One gateway** in the stool room (Waveshare ESP32-S3 RS485/CAN board, external antenna) holds every
  Bluetooth device — the three battery packs, the Power Watchdog, the Renogy solar controller and the
  EasyTouch thermostat — and publishes them as the DGNs below. It is also the **only ESP-NOW node**.
- **Two kinds of LCD panel:** *CAN panels* (`mid_coach`, `main_cabinet`) use only the bus, with no WiFi
  at all; *broadcast panels* (`bedroom_remote`) use only ESP-NOW, to and from the gateway, with the
  station ID of issue #95.
- **Valves:** any panel controls the dump valves. CAN panels send `0FFCE`; broadcast panels send a
  station-ID broadcast; the gateway alone talks to the valve node, over encrypted unicast.

These DGNs are **our own, for this coach**: they are not added to the G6's world, and they follow the
RV-C *conventions* (8-byte frames, instance in byte 0, `0xFF` for not-available) but not its message
catalogue.

Non-goals: no address claiming (none seen on this bus); no change to the dimmer/tank DGNs; commands to
Bluetooth devices other than the thermostat (battery FETs, solar load output — owner decision, the Gen 1
Watchdog cannot be commanded at all); no encryption on the wired bus.

## 2. The block

**`0x0FFC0`–`0x0FFCF`** (16 DGNs). J1939 proprietary-B space: PDU2, data page 0, PF `0xFF`.

| evidence | status |
|---|---|
| Never seen on the live bus (15 min survey) | all 16 |
| Transmitted for 12 min at realistic rates, G6 and others unchanged | `0FFC0`–`0FFC7` (issue #96) |
| Checked free on the bus but **not yet transmitted** | `0FFC8`–`0FFCF`: the probe now sends all 16 at the rates in sec. 5 (issue #99); run it at the coach before first use |

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

### 0FFC0 — HVAC zone status (producer: gateway, 2 s per zone)

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

### 0FFC1 — HVAC zone extras (gateway, 10 s per zone)

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

### 0FFCA — HVAC command (any CAN panel → gateway, event driven)

| byte | field |
|---|---|
| 0 | zone |
| 1 | op: 0 mode, 1 cool setpoint, 2 heat setpoint, 3 auto heat setpoint, 4 auto cool setpoint, 5 cool fan, 6 heat fan, 7 auto fan, 8 fan-only |
| 2 | argument (u8) |
| 3–7 | reserved |

The gateway applies it through the existing `easytouch_client_submit_change()`. The next status is the
confirmation, as today; panel UIs keep their optimistic-setpoint behaviour. A decoder rejects a zone
above 3 or an unknown op, so a malformed frame is never acted on.

### 0FFC2 / 0FFC3 / 0FFC4 — battery pack 1 / 2 / 3 (producer: gateway, 5 s each)

One DGN per pack. Byte 0 repeats the pack number (1..3).

| byte | field |
|---|---|
| 0 | pack 1..3 |
| 1 | flags: bit0 online, bit1 temperatures valid |
| 2 | state of charge, % |
| 3–4 | volts, u16, 0.01 V |
| 5–6 | current, s16, 0.1 A, **positive = charging** |
| 7 | reserved |

### 0FFC8 — battery pack detail (gateway, 10 s per pack)

| byte | field |
|---|---|
| 0 | pack 1..3 |
| 1–2 | residual capacity, u16, 0.1 Ah |
| 3–4 | full capacity, u16, 0.1 Ah |
| 5 | lowest temperature, compact °C |
| 6 | highest temperature, compact °C |
| 7 | reserved |

The per-pack DGNs plus this one carry everything the pack-detail popup and `jbd_bms_combine()` need.

### 0FFC5 / 0FFC9 — battery bank aggregate (gateway, 5 s each)

Computed on the gateway with the **same shared pure-C `jbd_bms_combine()`** the panels use, so the
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

### 0FFC6 — shore power line (gateway, 2 s per line)

Line A and line B, so the load split is visible. Byte 0 is the line.

| byte | field |
|---|---|
| 0 | line: 0 = A (L1), 1 = B (L2) |
| 1–2 | volts, u16, 0.1 V |
| 3–4 | amps, u16, 0.1 A |
| 5–6 | watts, u16, 1 W (saturates at 65534) |
| 7 | frequency, `Hz = 40.0 + raw × 0.1` (40.0–65.4 Hz, `0xFF` n/a) |

### 0FFCC — shore power pedestal (gateway, 10 s)

| byte | field |
|---|---|
| 0 | 0 |
| 1 | line count: 1 = 30 A, 2 = 50 A (line B is sent only when 2) |
| 2 | Watchdog error code (0 OK, 1–9 = E1–E9, 11/12 = F1/F2) |
| 3–7 | reserved |

### 0FFC7 — solar A / 0FFCB — solar B (gateway, 5 s each)

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

### 0FFCD — valve status (gateway, on change + 15 s)

The gateway relays what the valve node reports over ESP-NOW. Byte 0 is the valve.

| byte | field |
|---|---|
| 0 | valve: 0 = grey, 1 = black |
| 1 | position: 0 unknown, 1 closed, 2 open (anything else decodes as unknown) |
| 2 | age of the node's last report, seconds (saturates at 254; `0xFF` n/a) |
| 3 | flags: bit0 valve-node link alive |
| 4–7 | reserved |

### 0FFCE — valve command (any CAN panel → gateway, event driven)

| byte | field |
|---|---|
| 0 | valve: 0 = grey, 1 = black |
| 1 | action: 0 close, 1 open |
| 2–7 | reserved |

A dump valve moves on this, so the decoder accepts only an exact, in-range frame. The panel keeps its
arm-then-open UX; the valve node keeps its own interlock, 2 s drive ceiling and 3 s re-drive lockout, so a
gateway fault can stop commands but never hold a motor on.

### 0FFCF — gateway heartbeat (gateway, 5 s)

Lets a panel tell "this device dropped" from "the gateway is gone".

| byte | field |
|---|---|
| 0 | gateway id |
| 1 | firmware major |
| 2 | firmware minor |
| 3–4 | uptime, u16, minutes (saturates at 65534) |
| 5 | link bitmap: bit0–2 packs 1–3, bit3 Watchdog, bit4 solar, bit5 thermostat, bit6 valve node |
| 6 | health flags (gateway-defined) |
| 7 | reserved |

The block is full. Any future DGN (for example battery or solar commands) needs `0FFD0` or later, and a
probe run first.

## 5. Rates and bus load

| stream | frames/s |
|---|---|
| HVAC status 3 zones / 2 s, extras 3 / 10 s | 1.8 |
| Battery packs 3 / 5 s, detail 3 / 10 s, bank A+B 2 / 5 s | 1.3 |
| Shore lines 2 / 2 s, pedestal 1 / 10 s | 1.1 |
| Solar A+B 2 / 5 s | 0.4 |
| Heartbeat 1 / 5 s, valve status 2 / 15 s | 0.3 |
| **Total** | **about 4.9** |

About 0.3 % of 250 kbps, on top of the measured ~0.5 %. The probe already ran ~10 fps for 12 minutes;
its stage 2 now replays these rates (about 5 fps, with the commands once per 10 s).

## 6. Source addresses

Panels keep `0x80 + PANEL_INDEX`. The gateway joins the bus as a headless node, so `panels/REGISTRY.md`
gains a *headless CAN nodes* table: **gateway = index 5, `0x85`**. Index 4 (`hvac_panel`) is retired, not
reused. Observed on this coach, never to be used: `0x8F` (G6), `0x9C`–`0x9F` (wall switch panels), `0xAF`
(tank sensor). So indices must stay below 15.

## 7. Who produces and who consumes

| node | role | change |
|---|---|---|
| **gateway** (stool room, new) | **produces** everything in the block except the two commands; **consumes** `0FFCA` and `0FFCE` | holds all six BLE devices; the only ESP-NOW node: station-ID broadcast bridge for broadcast panels (lights, tanks, climate, power, solar, valves) and encrypted unicast to the valve node |
| `mid_coach` | consumes everything, sends `0FFCA` / `0FFCE` | becomes a **CAN-only panel**: the ESP-NOW bridge and valve link move to the gateway; WiFi removed |
| `main_cabinet` | consumes everything, sends `0FFCA` / `0FFCE` | CAN-only: ESP-NOW removed (tanks already arrive on CAN); gains dump-valve buttons |
| `bedroom_remote` | broadcast panel | station-ID broadcast to and from the gateway, including valve commands (owner decision) |
| valve node | unchanged logic | its single peer becomes the gateway; accepts valve frames **only from that MAC** |
| `proxy`, `hvac_panel` | retired | kept as rollback until v2 is verified |

⚠️ **The source-MAC check is a prerequisite, not a nicety.** `espnow_recv_cb` ignores the sender today, so
the valve node acts on a valve frame from any device. Once broadcast panels send valve requests, a frame of
the wrong type reaching the valve node directly would move a valve.

CAN panels decode in `twai_rx_task` and hand frames to a queue, then call the same
`ui_on_battery_status()` / `ui_on_shore_power()` / `ui_on_solar_status()` / `ui_on_hvac_status()` /
`ui_on_valve_status()` entry points the ESP-NOW telemetry feeds today. Widgets never parse frames.

## 8. Implementation shape

- **`components/firefly_dgn`** (done, issue #100): encode and decode for every layout above, floats in
  engineering units with `NAN` for not-available, rounding to the nearest step and saturating at the field
  limits. No ESP or easytouch/jbd dependencies; host-tested with golden byte vectors in CI.
- **`components/rvc_link`** (gateway): TWAI init, rx/tx tasks, dimmer and tank tables, DGN dispatch. The
  installed panels keep their own copies of that code.
- **`gateway/`** project, evolved from `proxy/`: ESP32-S3, BLE 4.2 host (the shared scan arbiter needs it),
  link counts sized for six, a node-wide connect arbiter in `ble_host`, and a Kconfig enable per function so
  it can be brought up one function at a time.
- The survey/probe build verifies each producer from `mid_coach`, which hears other nodes but not itself.
- Docs sweep as usual (`SYSTEM.md` diagram, `FLASHING.md`, `CHANGELOG.md`, `REGISTRY.md`).

## 9. Test plan

**Host (in CI):** one golden byte vector per DGN; round trip encode→decode; n/a, saturation both ways,
negative current, temperature rounding at the scale boundaries, 30 A single line, pack offline, bank with
2 of 3 packs; command decoders reject bad zones, unknown ops, out-of-range valves and actions, and the
probe's `0xFF` instance.
**Simulator:** feed frames into the panel UIs (extend the fake bus) and capture CLIMATE / POWER / BATTERY /
TANKS next to the ESP-NOW-fed originals.
**Bench:** the gateway holds six BLE links with the connect arbiter; `mid_coach`'s survey build confirms each
frame at its rate and byte values; a CAN panel sends `0FFCA` and the real thermostat changes; unplugging the
thermostat link clears the fresh flag; the valve node rejects a valve frame from a non-peer board.
**G6 safety:** survey baseline taken before each producer is enabled and `d` shows 0 deltas with it running;
abort rules as in the probe.
**Regression:** a broadcast panel shows the same data it does today; light control from every panel unchanged.

## 10. Open items

1. Transmit-test `0FFC8`–`0FFCF` at the coach (issue #99).
2. Single-box feasibility test in the stool room: six BLE links, then ESP-NOW alongside (#98 step 2). If it
   fails, fall back to two boards (basement BLE + stool-room thermostat/ESP-NOW) with the same code.
3. RV-C drop and a fused 12 V feed at the stool room.

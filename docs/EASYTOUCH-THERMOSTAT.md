# Micro-Air EasyTouch RV thermostat — BLE gateway (designed, NOT built)

Research and design record for controlling the coach's EasyTouch RV
thermostat from the panels. Written 2026-09-06 from three independent
open-source implementations of the same reverse-engineered protocol plus
Micro-Air's own manuals; nothing here has been bench-verified on this coach
yet. The **Bench plan** at the end lists what must be captured before a
line of firmware is written.

Sources (all read in full, not summarised from search snippets):

- [k3vmcd/ha-micro-air-easytouch](https://github.com/k3vmcd/ha-micro-air-easytouch)
  — the Home Assistant integration (Python/bleak). Most complete; multi-zone.
- [mbhewitt/easytouch](https://github.com/mbhewitt/easytouch) — the original
  proof-of-concept the HA integration grew from. Includes a GATT dump.
- [justAnotherDev/node-red-contrib-easytouch-ble](https://github.com/justAnotherDev/node-red-contrib-easytouch-ble)
  — older Node.js implementation; documents the auth **readback**.
- Micro-Air: [Operations Manual (all models)](https://www.micro-air.com/support-documents/EasyTouchRV/EasyTouch_RV_Thermostats_Operations_Manual_All.pdf),
  [350 manual rev 2.0](https://www.micro-air.com/support-documents/EasyTouchRV/EasyTouchRV_350_Manual_2.0.pdf),
  KB: [Bluetooth issues](https://www.micro-air.com/kb-easytouchrv/articles_troubleshooting/easytouchrv_bluetooth_issues.htm),
  [account connection issues](https://www.micro-air.com/kb-easytouchrv/articles_troubleshooting/easytouchrv_account_connection_issues.htm).

## 1. Short answer

Yes. The thermostat exposes a plain GATT service with three characteristics
and speaks **JSON over BLE**. Authentication is a single write of the
account password. Everything a panel needs (per-zone mode, fan, setpoints,
inside temperature) is one status read. The same BLE-central pattern already
used for the batteries, Watchdog and Renogy (`components/ble_host` +
Bluedroid GATTC) covers it.

Two things are unknown and can only be captured on the coach:

1. the **mode numbers for Aqua-Hot (auxiliary heat)** and for
   heat-pump / heat-strip on a 3-zone unit — no public implementation has
   them (HA integration issue #32 is open on exactly this), and
2. whether the thermostat **notifies** on the status characteristic or must
   be read after every request (every implementation reads; none subscribes).

Neither blocks the design; both are a 10-minute capture with the probe
script in §8.

## 2. "Pairing" — what it actually is

⚠️ **There is no BLE pairing or bonding.** The manual says so outright:
*"The smart device and display do not 'Pair' like other common devices."*
There is no LTK, no passkey, no bond table on the thermostat. Any central
can connect; it just cannot *do* anything until it writes the right password.

What the user has been calling "paired to a single device" is really two
separate rules:

| Rule | What it means for us |
|---|---|
| **One account per thermostat.** The thermostat stores the **password of the Micro-Air account** that first connected. Every phone that wants in must use that same account. (350 manual p.21: *"Each display can only be assigned to a single account, but many users can control the display if they use the same account."*) | The gateway needs the **account password string**, nothing else. It is not "a device"; it is just another client presenting the same password the phone app does. |
| **One BLE connection at a time.** The thermostat is a single-connection peripheral. | While the gateway is connected the phone app cannot connect, and vice-versa. Same constraint the Watchdog has. See §6 for how the gateway holds the link. |

The **Bluetooth password reset** on the thermostat (Settings → Bluetooth
icon → reset password) does **not** un-pair anything. It clears the *stored
password* so that the next password written over BLE is accepted and saved.
You need it only when the stored password no longer matches the account
password (e.g. after changing the account password in the app), or if you
want to move the thermostat to a different account. If the phone app works
today, the gateway will work with that same password and **no reset is
needed**.

### First-connection procedure for the gateway

1. Confirm the phone app can connect over Bluetooth (not WiFi) right now.
   If it can, the stored password == the account password; skip to step 4.
2. If the phone app cannot connect: on the thermostat, Settings gear →
   flip pages to the **Bluetooth** icon → **reset password**. Then open the
   phone app, connect once over Bluetooth. That write re-stores the
   account password. (Micro-Air KB steps 6–8.)
3. Confirm the app connects and shows live data.
4. Put the **account password** (and the account e-mail, see §3.3) into the
   gateway's Kconfig (`FIREFLY_EASYTOUCH_PASSWORD`, per-build-dir sdkconfig,
   same handling as the battery MACs — never in `sdkconfig.defaults`).
5. Kill the phone app (background is not enough on iOS — force-quit, it
   holds the connection).
6. Boot the gateway. It scans by name (`EasyTouch nnnnnnnnn`, where the
   digits are the serial number; first three digits = model number), connects,
   writes the password, reads the auth characteristic back and expects
   `Matched…`, then requests status. If the readback is anything else the
   password is wrong: log it, disconnect, retry with backoff — **do not
   hammer it**, the thermostat has been reported to need a power-cycle after
   repeated bad auth.
7. The phone app will still work later: the gateway releases the link
   between polls (§6), so the app can get in whenever it wants, and the
   gateway simply reconnects afterwards.

There is no step where the thermostat has to be "told about" the gateway.

## 3. Wire protocol

### 3.1 GATT

Advertised name `EasyTouch <serial>`. Primary service and characteristics
(from mbhewitt's `bluetoothctl` dump, confirmed by the other two):

| UUID (16-bit, base `0000xxxx-0000-1000-8000-00805F9B34FB`) | Role | Access |
|---|---|---|
| `00FF` | the EasyTouch service | — |
| `DD01` | **password / auth** | write, then read back |
| `EE01` | **JSON command in** | write |
| `FF01` | **JSON status out** | read (notify: unknown, see bench plan) |
| `2A05` (in `1801`) | Service Changed | standard, ignore |

The node-red library also filters its scan on advertised service UUID
`0x0D18`; the HA integration and mbhewitt match by name. Name match
`EasyTouch` prefix is enough for a coach with one thermostat, but the design
pins the MAC after first discovery like the Renogy client does.

### 3.2 Authentication

Write the account password as raw UTF-8 bytes (no JSON, no terminator) to
`DD01` with response. The node-red library then **reads `DD01` back** and
requires the string to start with `Matched`; the HA integration skips the
readback and just treats the write's success as auth (it therefore cannot
tell a wrong password from a right one — see its issue #36 "Unable to
control after adding"). The gateway will do the readback.

Auth is per connection. Every reconnect re-authenticates.

### 3.3 Status request / response

Write to `EE01`:

```json
{"Type":"Get Status","Zone":0,"EM":"<account e-mail>","TM":1725600000}
```

then read `FF01`. `Zone` in the request is ignored for content — the
response carries **all** zones. `EM` and `TM` are what the phone app sends;
`TM` is a Unix time and is how the thermostat learns the time of day (the
manual: *"Time is pulled in when a Bluetooth or WiFi connection is made from
the app"*). The node-red client omits both and still gets status.

⚠️ **The gateway has no clock. Omit `TM` (and `EM`) rather than send a
bogus value** — a wrong `TM` would set the thermostat's clock and the
schedule feature runs off it. Bench plan item: confirm the response is
identical without them.

The same message with `LAT`/`LON` strings (`"%.5f"`) sets the location used
for weather/time zone; not needed.

Response (JSON, well over one MTU — see §6):

```json
{"SN":"350xxxxxx", "PRM":[...], "Z_sts":{"0":[16 ints], "1":[16 ints], "2":[16 ints]}, ...}
```

`Z_sts` keys are zone indices as strings; a zone that is present has a
16-element int array. That is how zone count is discovered — the HA
integration probes exactly this way and then creates one climate entity
per key.

Per-zone array layout (HA integration + mbhewitt, identical):

| idx | field | notes |
|---|---|---|
| 0 | `autoHeat_sp` | °F, low setpoint in AUTO |
| 1 | `autoCool_sp` | °F, high setpoint in AUTO |
| 2 | `cool_sp` | °F |
| 3 | `heat_sp` | °F |
| 4 | `dry_sp` | °F |
| 5 | unknown | |
| 6 | fan setting, **fan-only mode** | 0 off / 1 low / 2 high |
| 7 | fan setting, **cool mode** | full enum below |
| 8 | unknown | |
| 9 | fan setting, **auto mode** | full enum |
| 10 | **`mode`** — the selected mode | enum below |
| 11 | fan setting, **heat mode** | full enum |
| 12 | **inside temperature** (faceplate, °F) | the value the panels will show |
| 13, 14 | unknown | |
| 15 | **`current_mode`** — what is actually running now | enum below |

`PRM` is a list of ints; `7 in PRM` ⇒ system off, `15 in PRM` ⇒ system on
(both implementations use it that way; the node-red client's older firmware
saw a scalar `parmFlags == 44` for "on" instead, so treat as version-
dependent and log the raw list on the bench).

Mode enum, as far as anyone has decoded it:

| value | meaning | source |
|---|---|---|
| 0 | off | all three |
| 1 | fan only | all three |
| 2 | cool | all three |
| 3 | ~~cool, compressor running~~ → **not observed; §8c shows `current_mode = 2` (same as `mode`) while actively cooling** | mbhewitt, HA assumed 3; corrected 2026-09-06 |
| 4 | ~~heat (electric: heat pump **or** heat strip)~~ → **confirmed on this coach: Aqua-Hot selected** | all three assumed "heat"; §8b bench capture 2026-09-06 corrects to Aqua-Hot |
| 5 | heat, **running** (`current_mode` only) | HA — not confirmed live; this coach's `current_mode` used 4, not 5, while heating (§8b) |
| 6 | dry | HA (as a *command*; never seen as status) |
| **7** | **Electric heat selected — labeled "Heat Pump" or "Heat Strip" per zone**, the touchscreen picking the label from that zone's own A/C board config, same one protocol value either way | **confirmed on this coach, §8b/§8c bench captures 2026-09-06 (Front/Mid Coach show "Heat Pump", Rear shows "Heat Strip", all mode 7) — not in any upstream source, answers HA issue #28** |
| 11 | auto (heat/cool with dual setpoints) | all three; not yet confirmed live on this coach |
| ? | auto combined with heat pump specifically | still open — HA issue #28's remaining half; needs a live Auto capture |
| ? | 14 = "heat running"? | node-red's older firmware only; not seen on this coach |

The 350 touchscreen offers per zone: Off, Fan, Cool, **Heat Pump *or* Heat
Strip** (whichever the zone board's DIP switches declare — never both; the
manual lists "heat strip and heat pump both set" as `INVALID CONFIG`),
**Furnace *or* Aqua** (same output, icon chosen by the Furnace/Aqua setting;
on the 350 the manual says the choice *"should be set appropriately since
there is some function associated with the selection"*), Auto, and the
schedule. So on this coach each zone should expose: Off / Fan / Cool /
Heat (strip) / Aqua / Auto. Whether Auto can be "cool + Aqua" vs
"cool + strip" is a touchscreen question to answer while capturing.

Fan enum (idx 7/9/11, and the `coolFan`/`heatFan`/`autoFan` command keys):

| value | meaning |
|---|---|
| 0 | off |
| 1 | manual low |
| 2 | manual high |
| 65 | cycled low (runs with the cycle only) |
| 66 | cycled high |
| 128 | full auto |

Fan-only mode (idx 6 / `fanOnly` key) uses just 0 / 1 / 2.

### 3.4 Commands

All commands are one JSON write to `EE01`:

```json
{"Type":"Change","Changes":{"zone":<n>, ...}}
```

Known `Changes` keys:

| key | value | effect |
|---|---|---|
| `power` | 0 / 1 | ⚠️ **system-wide** on/off, not per zone (HA issue #37 — setting one zone "off" with `power:0` shut down both A/C units). Per-zone off is `mode:0` with `power:1`. |
| `mode` | mode enum | select mode for this zone |
| `cool_sp`, `heat_sp`, `dry_sp` | °F int | setpoint for that mode |
| `autoHeat_sp`, `autoCool_sp` | °F int | AUTO dual setpoints |
| `coolFan`, `heatFan`, `autoFan` | fan enum | fan for that mode |
| `fanOnly` | 0/1/2 | fan speed in fan-only mode |
| `reset` | `" OK"` (leading space) | reboots the thermostat; the link drops with GATT error 133, which is expected |
| `sched_sp` | °F | node-red's older firmware; probably superseded — not used |

Several keys may go in one message (mbhewitt sends
`power, mode, cool_sp, coolFan` together). The HA integration sends
`{"zone":n,"power":1,"mode":m}` for a mode change and
`{"zone":n,"power":1,"cool_sp":t}` for a setpoint — the `power:1` there
is the global-power trap above and the gateway should **not** copy it
blindly: send `power:1` only when the system is currently off and the user
asked for a running mode.

There is no command acknowledgement. Confirmation is the next status read,
exactly like RV-C `DC_DIMMER_STATUS_3` — the panels' existing
"visual state comes only from status" invariant applies unchanged.

## 4. Where it runs

**A new headless node, `hvac/`, not the proxy and not a panel.** Same
pattern and reasoning as `valves/`:

- The proxy is at **5 of 6** BLE links (`BTDM_CTRL_BLE_MAX_CONN=6`). A sixth
  fits numerically, but the proxy runs the peerless TELEMETRY role, and
  thermostat *commands* must be **encrypted unicast** (they actuate real
  loads; broadcast is unencrypted by design). Giving the proxy a unicast
  peer changes a node that has been swapped twice already and whose failure
  mode today is merely stale readouts. A hung node that owns the furnace
  should be its own box.
- Range: the thermostat is on an interior wall; the proxy is in the
  basement bay behind a steel door (the reason the batteries left the
  bedroom panel in the first place, in reverse).
- A panel cannot hold it either: the 4.3B panels' radio is already carrying
  ESP-NOW, and the 7B panel has no PSRAM headroom to spare for Bluedroid.

Any ESP32 works. A classic ESP32 like the proxy (with heatsink) keeps one
`try_connect()` guard style in play; an S3 works too. Either way the client
selects the connect API on `CONFIG_BT_BLE_50_FEATURES_SUPPORTED` like the
other two clients.

### 4.1 Traffic classes

| direction | class | why |
|---|---|---|
| gateway → everyone | **status broadcast**, `ESPNOW_TELEM_HVAC`, one 16-byte frame per zone, every poll | read-only, so unencrypted broadcast is acceptable — exactly like battery/shore/solar. Every panel that wants a thermostat readout gets it for free, including `main_cabinet` in its peerless TELEMETRY role, with **no new peer configuration anywhere**. |
| panel → gateway | **command**, encrypted unicast, new frame type `ESPNOW_FRAME_HVAC_CMD` | actuates a real load. |

Command routing follows what issue #64 establishes for the valves: the
gateway's one unicast peer is `mid_coach`; `mid_coach` adds it as a further
peer and relays. A `bedroom_remote` tap already goes to `mid_coach` for
lights, so the thermostat path is the same hop count. Whatever #64 decides
for `main_cabinet` originating valve commands (it has no unicast peer
today) is inherited here unchanged.

⚠️ `espnow_link_add_valve_peer()` is a one-off second-peer function. A third
peer should turn that into a small peer table rather than a third copy —
worth doing inside #64 while it is still open.

### 4.2 Proposed wire structs (16 bytes each, fits the existing envelope)

```c
/* gateway -> broadcast, one per zone per poll */
typedef struct {
    uint8_t zone;          /* 0..3 */
    uint8_t flags;         /* bit0 present, bit1 system on, bit2 gateway linked */
    uint8_t mode;          /* raw EasyTouch mode enum, idx 10 */
    uint8_t current_mode;  /* raw, idx 15 */
    uint8_t fan;           /* raw fan enum for the selected mode */
    int8_t  inside_f;      /* idx 12 */
    uint8_t cool_sp, heat_sp, auto_heat_sp, auto_cool_sp, dry_sp;
    uint8_t reserved[5];
} espnow_hvac_status_msg_t;

/* panel -> gateway, unicast */
typedef struct {
    uint8_t zone;
    uint8_t op;            /* SET_MODE, SET_FAN, SET_SETPOINT, SET_POWER */
    uint8_t arg0, arg1;    /* mode / fan / which-setpoint + value */
} espnow_hvac_cmd_msg_t;
```

Raw EasyTouch enums go on the wire on purpose: the aux-heat number is not
known yet, and a codec that maps it can be fixed once in the pure-C
protocol component rather than at both ends. Panels display via a lookup
table that lives in `components/easytouch` next to the JSON codec.

## 5. Component split

`components/easytouch/`, same shape as `jbd_bms` / `renogy_solar`:

- `easytouch_protocol.c` — pure C, no ESP deps: builds the `Get Status` and
  `Change` JSON strings (a fixed-format `snprintf`, no JSON library needed
  on the TX side), parses the status response into
  `easytouch_status_t { zones[4], present_mask, system_on }`. The parser
  needs to walk one level of JSON with integer arrays; `cJSON` is in IDF
  and is fine on the gateway, but a host test still has to run — vendor a
  minimal scanner for `"Z_sts":{"<n>":[...]}` and `"PRM":[...]` instead, so
  `host_test/` compiles with plain `gcc` like the others. Regression vector
  = the first real status capture from this coach.
- `easytouch_client.c` — Bluedroid GATTC state machine
  (scan-by-name via `ble_host_scan_add_matcher()` → connect → discover
  `00FF` → write `DD01` → read `DD01`, expect `Matched` → poll loop),
  registered through `ble_host` with a new app id
  (`BLE_HOST_APP_ID_EASYTOUCH 30`). Name match should be **prefix**
  `EasyTouch` (unlike the Renogy's exact match, a neighbour's thermostat is
  distinguishable by serial and the MAC is pinned after first hit anyway).

`hvac/` project: `ble_host` + `easytouch` + `espnow_link`, own
`partitions.csv` (Bluedroid + WiFi does not fit the 1 MB default — copy the
proxy's), own `sdkconfig` holding the password.

## 6. Connection policy and BLE details

- **Connect → auth → status → disconnect, every poll**; do not hold the
  link. That is what the HA integration does (`_send_command_locked` always
  disconnects in `finally`), and it is what keeps the phone app usable.
  A command is: connect, auth, write `Change`, write `Get Status`, read,
  broadcast, disconnect — so a command costs one extra connection, not a
  standing one. Poll interval 20–30 s (`FIREFLY_EASYTOUCH_POLL_INTERVAL_MS`,
  same Kconfig-derived staleness window trick as `JBD_HEALTHY_WINDOW_MS`).
  If the bench shows `FF01` notifies, the poll can become "hold the link
  and subscribe" later — but then the app is locked out while the gateway
  is up, which is the trade-off the user has to choose, not the firmware.
- **MTU.** A 3-zone status is several hundred bytes. Request a 512 MTU
  right after connect (`esp_ble_gattc_send_mtu_req`, needs
  `esp_ble_gatt_set_local_mtu(512)` before). Bluedroid does long reads
  (read-blob) and prepare-writes automatically for values longer than
  MTU−3, but that is precisely the thing to verify on the bench: if the
  status comes back truncated at 20 or 23 bytes, the MTU request is the
  first suspect.
- **Auth failure is silent on write.** Only the `DD01` readback tells. On a
  non-`Matched` readback: log the reply, disconnect, back off (1 s, 2, 4 …
  cap 60 s). Never retry in a tight loop.
- **Phone app collision.** While the app holds the link, the gateway's
  connect attempt fails (status 133 / `reason 0x100`, same as an offline
  pack on the bench). Treat as "busy", keep the last status, mark the zone
  frames' *linked* flag false after 3 misses so the panels can age the
  readout to `--`, exactly like shore power does after 20 s.
- **`power` is global.** Panel UI must not offer a per-zone "off" that maps
  to `power:0`. Per-zone off = `mode:0`. "System off" is a separate,
  deliberate button if it is wanted at all.

## 7. Panel UI (to decide with the user)

Not designed here; needs the mode capture first. Placeholder shape:
one CLIMATE section on `main_cabinet`'s rail and one screen on
`bedroom_remote`, three zone cards each showing inside °F, mode, fan,
setpoint, with − / + on the setpoint and a mode cycle button. Which zones
are which rooms comes from the touchscreen's zone names (not in the
protocol — capture by reading the screen).

## 8. Bench plan — do this BEFORE any firmware

The probe script (`tools/easytouch_probe.py`, bleak, runs on this PC's
Bluetooth or any laptop parked in the coach) does the whole sequence and
dumps raw JSON. It needs `pip install bleak`.

1. **Auth.** Run `probe.py --password …`. Expect `DD01` readback starting
   `Matched`. Record the exact string. Then run once with a wrong password
   and record that reply too — that is the negative test the firmware will
   key on.
2. **Status without `EM`/`TM`.** Run `--no-em-tm`; diff the JSON against a
   run with them. Confirms the gateway can omit both.
3. **Full raw status capture** → `components/easytouch/host_test/`
   regression vector. Note `SN`, the `PRM` list, and the number of `Z_sts`
   keys (expect 3).
4. **Mode capture.** For **each** zone, on the touchscreen select in turn:
   Off, Fan, Cool, Heat (strip), **Aqua**, Auto; after each, run
   `--status` and record `Z_sts[z][10]` and `[15]`. This fills the two `?`
   rows in §3.3. Also note whether Auto on this coach means cool+strip or
   cool+Aqua, and whether the Furnace/Aqua toggle changes the number.
5. **Fan capture.** Same for each fan choice in cool and heat.
6. **Notify test.** `--watch 60`: subscribe to `FF01` and change the
   setpoint on the touchscreen. If anything arrives unsolicited, the
   thermostat pushes; otherwise it is read-only and the poll design stands.
7. **Per-zone off.** With all three running, send
   `{"zone":1,"mode":0}` and confirm zones 0 and 2 keep running; then
   `{"zone":1,"power":0}` and confirm (or refute) that it kills everything.
   Send `power:1` afterwards.
8. **App coexistence.** With the probe polling every 20 s, open the phone
   app: it should connect within one poll gap. Close it; the probe recovers.
9. **Zone names / rooms** from the touchscreen, for the panel labels.

Everything in §3.3 marked *unknown* becomes a table row here, and the
captured frames go into the host test the same way the JBD doc frame and
the three SeeLevel frames did.

## 8a. First live capture (2026-09-06, coach, `hvac_capture` on COM23)

The read-only bench tool (`hvac_capture/`, issue #70) connected, authenticated,
and read status from the real thermostat on the first attempt. Corrections
to the sections above, from the actual response:

- **`SN` is `355003525`** — this coach's thermostat is a **355** (Coleman
  Mach zone-control replacement), not a 350. The 355 is the multi-zone A/C
  model (no CCC2-specific "set which output" quirk the 350 manual calls
  out); treat the 350 manual's zone-board-DIP-switch description as
  informative context only, not literal to this unit.
- **The response has more top-level fields than any of the three source
  implementations documented**, all new: `"Type":"Response"`,
  `"RT":"Status"`, `"TT":"EasyTouch"`, `"REV":"1.0.7.0"` (firmware
  revision), `"alertLL"`/`"alertUL"` (the notification limits from settings),
  `"CI"`, `"hA"`. None of these change the zone-array layout in §3.3; they
  are cosmetic/metadata additions in this firmware revision.
- ⚠️ **`PRM` is NOT the two-flag list §3.3 assumed.** Captured value:
  `"PRM":[0, 107, 67, 76]`. Neither `7` nor `15` appears, so the
  `7 in PRM ⇒ off` / `15 in PRM ⇒ on` rule from the HA integration and
  mbhewitt does not hold on this firmware revision (`1.0.7.0`). System
  on/off state will have to come from somewhere else — likely a per-zone
  `current_mode` of 0 already covers "this zone is off" without needing
  `PRM` at all; whether there is a true global on/off flag anywhere in this
  response is still open. Do not implement the `PRM` on/off rule as
  written until this is resolved.
- **First real zone arrays**, all three zones in Cool with fan on auto,
  nothing actively running:

  | zone | raw array | inside °F | mode | current_mode | cool_sp | heat_sp | auto lo/hi |
  |---|---|---|---|---|---|---|---|
  | 0 (Front) | `[68,72,78,70,72,45,0,128,128,128,2,0,77,255,0,0]` | 77 | 2 (cool) | 0 (off/idle) | 78 | 70 | 68/72 |
  | 1 (Mid Coach) | `[68,72,75,72,72,45,0,128,128,128,2,0,75,255,0,2]` | 75 | 2 (cool) | **2** | 72 | 72 | 68/72 |
  | 2 (Rear) | `[68,72,76,70,72,45,0,128,128,128,2,0,77,255,0,0]` | 77 | 2 (cool) | 0 | 76 | 70 | 68/72 |

  Idx 5 = **45** and idx 13 = **255** on every zone here, and idx 8 = 128
  matching idx 7/9 — worth watching whether idx 5/8/13 ever change or are
  fixed/reserved on this unit. Zone 1's `current_mode = 2` while mode is
  also `2` (not `3`, the documented "cool, compressor running" value) is
  the first real data point on the `mode` vs `current_mode` distinction —
  with the AC not audibly running at capture time, `current_mode` may
  simply mirror `mode` when idle in a call-for-cool state rather than
  needing to reach setpoint first; needs a capture while the compressor is
  audibly running to confirm 2 vs 3.
- Zone ordering in `Z_sts` (`"0"`, `"1"`, `"2"`) matches the panel plan's
  Front / Mid Coach / Rear order (zone index = position in that list).
- Still open, exactly as before: the Aqua-Hot / heat-strip mode numbers.
  This capture only saw Cool; running the touchscreen through Off / Fan /
  Heat / Aqua / Auto per zone (bench plan step 4) while the capture tool's
  serial monitor is attached is the next session.

## 8b. Mode numbers captured (2026-09-06, Front + Mid Coach cycled)

Four more captures, cycling Front and Mid Coach (zones 0 and 1) between
their two selectable heat sources with the other two zones off:

| capture | zone changed | to | zone idx | `mode` (idx 10) | `current_mode` (idx 15) |
|---|---|---|---|---|---|
| 1 | Front | Heat Pump | 0 | **7** | 4 |
| 2 | (intended Mid, landed on Front — see note) | Aqua-Hot | 0 | **4** | 4 |
| 3 | Mid Coach | Heat Pump | 1 | **7** | 4 |
| 4 | Mid Coach | Aqua-Hot | 1 | **4** | 4 |

**Confirmed, updating §3.3's mode table:**

- **`mode = 7` is Heat Pump.** Not documented in any of the three upstream
  projects (none had a heat-pump zone) — this answers HA integration
  issue #28 for this protocol revision.
- **`mode = 4` is Aqua-Hot**, not the generic "heat" the HA integration and
  mbhewitt assumed. This coach's zone boards apparently offer exactly two
  selectable heat sources per zone — Heat Pump and Aqua-Hot — with no
  separate heat-strip button appearing on the touchscreen for either zone
  tested; the "heat bar" in the A/C unit is most likely the heat pump's own
  built-in resistive backup rather than an independently switchable
  EasyTouch mode. If a heat-strip-only mode number turns out to exist, it
  hasn't been seen yet.
- **`current_mode = 4` while actively heating, regardless of which mode
  (4 or 7) is selected.** So `current_mode` on this firmware is a generic
  "calling for heat" indicator, not a per-source value — it does not
  distinguish Aqua-Hot from Heat Pump the way `mode` does. (Off zones
  showed `current_mode = 0` throughout, consistent with §3.3.)
- **Capture 2 note:** the log shows the change landing on zone index 0
  (mode 4) rather than index 1, while the change was intended for Mid
  Coach. Captures 1, 3, and 4 are internally consistent (Front ↔ zone 0,
  Mid Coach ↔ zone 1, matching the plan's ordering), so this looks like a
  one-off UI navigation slip during capture — the touchscreen was probably
  still on the Front zone's screen — rather than a protocol surprise.
  Treat captures 1/3/4 as authoritative for the zone-index mapping.
- **Rear (zone 2) not yet directly tested**, but Front and Mid Coach agree
  exactly on both mode numbers (7 and 4) with identical A/C + Aqua-Hot
  equipment, and the enum is a firmware constant shared across zones, not
  something that varies per zone. Extending the same 7/4 pair to Rear is a
  safe working assumption for building the panel UI; a one-time spot check
  on Rear before flashing the real gateway is cheap insurance given
  capture 2's mix-up above, but is not expected to change anything.

**`PRM` still not resolved, but a pattern emerged.** All four values shift
together depending on which zone (0 or 1) is actively heating:

| captures | `PRM` |
|---|---|
| 1, 2 (zone 0 heating) | `[0, 107, 68, 79]` |
| 3, 4 (zone 1 heating) | `[1, 107, 67, 78]` |

`PRM[0]` flips 0↔1, and `PRM[2]`/`PRM[3]` each drop by exactly 1, based on
*which zone* is calling for heat rather than a simple system-wide on/off
bit. Not needed for any command this design sends (commands address a
zone directly), so it does not block anything — leave as an open curiosity
rather than a blocker.

Updated mode enum (§3.3), current state of knowledge:

| value | meaning |
|---|---|
| 0 | off |
| 1 | fan only |
| 2 | cool |
| 3 | ~~cool, running~~ — see §8c: `current_mode` reads **2**, same as `mode`, while cooling |
| **4** | **Aqua-Hot selected** (confirmed) / `current_mode`: generic "heating" |
| **7** | **Electric heat selected** — Heat Pump or Heat Strip, per-zone label (confirmed, see §8c) |
| 11 | auto (not yet confirmed live) |
| — | dry — never seen live |

## 8c. Rear spot-check (2026-09-06) — corrects "Heat Pump" to a per-zone label

Three more captures, Rear (zone 2) only, confirming and correcting §8b:

| capture | screen showed | zone idx | `mode` | `current_mode` |
|---|---|---|---|---|
| A/C on | (running) | 2 | 2 | **2** |
| Heat | **"Heat Strip"** | 2 | **7** | 4 |
| Aqua-Hot | "Aqua Hot" | 2 | 4 | 4 |

**Correction: `mode = 7` is not specifically "Heat Pump".** Rear's
touchscreen showed **"Heat Strip"** for the same mode value 7 that Front's
and Mid Coach's showed as "Heat Pump" in §8b. This is exactly the
per-zone-hardware behavior the manuals describe (*"may be a heat pump or a
heat strip depending on the factory zone set up"*) — `mode = 7` is the one
**electric heat** slot, and the touchscreen picks the Heat Pump vs Heat
Strip label per zone from the underlying A/C control board's own
configuration, not from anything the protocol communicates as a separate
value. So Rear's A/C is heat-strip-equipped where Front/Mid Coach's are
heat-pump-equipped — panel UI should follow whatever label the touchscreen
already uses per zone rather than assuming one label for all three.

**Correction: `current_mode` while actively cooling is `2`, not `3`.**
The "A/C on" capture shows `mode = 2, current_mode = 2` while the unit was
audibly running — the upstream `3 = "cool, compressor running"` value was
never observed on this firmware; `current_mode` appears to simply mirror
`mode` once a cycle is actively engaged, for both Cool and either heat
source (idx 15 = 4 while heating, matching §8b).

With this, **every mode the coach actually has is now captured live**:
Off, Fan (assumed unchanged from upstream, not separately re-tested), Cool,
Heat (Pump or Strip, per zone), Aqua-Hot. Auto remains the one mode never
seen live; not expected to be needed for the panel UI's initial scope.

**How to continue the capture** (per bench plan §8, steps 4-5, 7): run

```
idf.py -C hvac_capture -B hvac_capture\build -p COM23 monitor
```

then, on the physical thermostat touchscreen, cycle each zone through
Off → Fan → Heat → Aqua → Auto (and back to Cool), watching the log after
each change (poll interval is 5 s) and noting the `mode=` / `current=`
numbers that appear. `Ctrl+]` exits the monitor. Report the numbers back
(or paste the raw log) to fill in the `?` rows in §3.3.

## 9. Open decisions for the project owner

1. Board for `hvac/` (spare classic ESP32 with heatsink vs an S3 dev
   board). Either is fine; the client code guards on the BLE-5 symbol.
2. Poll-and-release (phone app keeps working, ~25 s latency on status)
   vs hold-and-subscribe (instant, app locked out) — default is
   poll-and-release.
3. Which panels get the climate UI, and what each zone is called.
4. Whether "system off" (`power:0`) is exposed on a panel at all.
5. Fold the third unicast peer into #64's peer table now, or open a
   separate issue for it.

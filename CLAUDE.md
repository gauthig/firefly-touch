# firefly-touch

Replacement touchscreen wall panels for a 2019 Entegra Aspire 44W with a
Firefly Integrations G6A multiplex system. Each panel is an **independent peer
node** on the coach's RV-C bus (CAN 2.0B, 250 kbps, 29-bit extended IDs).
There is no hub or gateway — the bus is the shared state mechanism.

Hardware: two board types, both ESP32-S3-WROOM-1 with 16 MB flash / 8 MB
octal PSRAM, GT911 touch on I2C, CH422G IO expander, TJA1051 CAN
transceiver, 7-36 V input:

- Waveshare **ESP32-S3-Touch-LCD-4.3B** — 4.3" 800x480, run rotated to
  portrait. `mid_coach`, `ent_center`, `bedroom_remote`.
- Waveshare **ESP32-S3-Touch-LCD-7** (non-B) — 7" 800x480 EK9716, run
  landscape. `main_cabinet`.

⚠️ **The boards put CAN on different pins** (4.3B: GPIO15/16; 7": GPIO20/19,
where the 4.3B has RS485) and a mismatch fails *silently* — the panel boots,
lights up, and never sees the bus. `BOARD` is therefore **derived from
`PANEL`** by a mapping in the root `CMakeLists.txt`; there is no `-DBOARD=`
to forget. `tools/check_panels.py` cross-checks it against
`panels/REGISTRY.md`'s Board column.

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
idf.py -B build_mid_coach -DPANEL=mid_coach build
idf.py -B build_ent_center  -DPANEL=ent_center  build
idf.py -B build_mid_coach -DPANEL=mid_coach -p COM5 flash monitor
```

Use one build dir per panel (PANEL is cached; switching values in a shared
build dir requires `fullclean`). Valid PANEL values = basenames of headers in
`panels/`. Panel identity: `PANEL_INDEX` → RV-C source address `0x80 + index`;
`mid_coach` = 0/0x80, `ent_center` = 1/0x81. Never reuse an index.

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
.\build.ps1 -Run                    # mid_coach, interactive window
.\build.ps1 -Panel ent_center -Run
.\build.ps1 -Shot preview.bmp       # headless screenshot, then exits
.\build.ps1 -Shot p.bmp -Screen2    # ...of screen 2
.\build.ps1 -Shot p.bmp -Screen2 -Popup   # ...with the pack-detail popup open
```

**The sim window is 480x800 (portrait), matching the firmware's LOGICAL
resolution** — `board_4_3b.c` runs the physically-800x480 panel rotated 90°,
and all UI layout code sizes itself off
`lv_display_get_vertical_resolution()`. The sim used to create an 800x480
landscape display, which previewed a screen the hardware never shows; it now
matches, so no rotation is needed on the sim side at all. Don't "fix" it back
to the physical dimensions.

Headless `--shot` renders into a full-size XRGB8888 framebuffer and writes
that, rather than calling `lv_snapshot_take()` on the active screen — a
snapshot walks one object tree and so silently misses everything on
`lv_layer_top()`, which is where the idle-dim overlay and the battery
pack-detail popup live.

Mouse = touch: click to toggle, click-and-hold to ramp. Requires the WinLibs
gcc (winget) and cmake/ninja (auto-sourced from ESP-IDF's export.ps1). SDL2
lives in `sim/third_party/` (gitignored; build.ps1 error tells you the
download if missing).

`sim_stubs.c` also fakes `TANK_STATUS` on `mid_coach` (a periodic sweep
through FRESH/GREY/BLACK percentages, `tank_sweep_timer_cb`) so the wave
gauges and the header's Grey-Black OK/Warn/FULL readout are visible without
real hardware. `--shot <file> screen2` (`main_sim.c`) taps the TANK LEVELS
button and runs real time forward briefly before snapshotting so both
timers get a chance to fire; `SIM_TANK_START_TICK=<n>` (env var) jumps the
sweep straight to a specific point (e.g. a FULL/blink frame) for a
one-off capture instead of waiting on it.

`sim_stubs.c` similarly fakes `jbd_bms_get_status()` on `bedroom_remote`
(`battery_sweep_timer_cb`) — battery 1 slow-charges 0→100→0, battery 2
slow-discharges the opposite phase, battery 3 sits idle at a fixed low SOC
**and drops offline for part of the cycle**, which is what exercises the
shrinking-bank path and the amber "2 of 3" indicator (otherwise only
reachable by physically powering down a pack on the bench). All three
report two NTC temperatures so the °F high/low readout is live too.
`SIM_BATTERY_START_TICK=<n>` jumps the sweep to a chosen point — same trick
and same reason as `SIM_TANK_START_TICK`, since a `--shot` capture only
runs ~2 s of real time and would never reach the offline window naturally.

There's no real BLE stack in the sim at all, and no client left to fake:
the panel is fed by ESP-NOW telemetry, so the sweep pushes through
`ui_on_battery_status()`, the same entry point the real broadcasts use.
(Historical note, since the file is gone: `sim/stubs/jbd_bms_client.h`
shadows the real component header (which pulls in the ESP `bt` component)
with just the declarations `ui.c` needs, backed by that fake table;
`jbd_bms_protocol.c` itself is compiled straight into the sim unmodified
(same host-testable pure-C pattern as `rvc_protocol.c`), so
`jbd_bms_combine()` runs there for free — the sim exercises the real
aggregation, not a fake of it. `--shot <file> screen2` taps whichever of
"TANK LEVELS" or "BATTERY STATUS" exists on the built panel; adding
`popup` also taps the bank readout to open the pack-detail popup.

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
| `jbd_bms`      | -    | 9    | **not on a panel any more** — runs on the basement proxy, which holds the battery BLE links and broadcasts the readings (see *Battery monitor* below) |
| LVGL (port)    | 1    | 4    | rendering + touch; button callbacks only enqueue via `bridge_enqueue_dimmer_cmd()` |

A CAN panel can also set `PANEL_WANTS_TELEMETRY 1` to run `espnow_rx` in the
receive-only TELEMETRY role (no unicast peer, no keys). `main_cabinet` does:
the battery packs and the Power Watchdog are on BLE links held by the
basement proxy, and no amount of CAN wiring reaches them.

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
- `components/board` — board bring-up: RGB timings, GT911, esp_lvgl_port
  glue, TWAI init. One component, one board compiled: `board_4_3b.c` or
  `board_lcd7.c`, chosen by `BOARD`, both behind `include/board.h` so `main/`
  and `sim/` never name a board. RGB timing / pin values follow the vendor
  demos — **merge points are marked in each file; diff against the current
  Waveshare demo (or Espressif's `ESP32_Display_Panel` board definition) when
  bringing up hardware, don't invent timings.**
  ⚠️ Only the SOURCE selection is conditional in its `CMakeLists.txt`;
  `REQUIRES` is a constant list. IDF resolves the dependency graph in an
  *early expansion* pass that runs before the project's variables exist, so
  anything conditional on `BOARD` there evaluates with `BOARD` unset. An
  earlier attempt used two components that each registered empty unless
  selected; every `REQUIRES` silently became nothing and the build failed
  with "board.h: No such file" while the CMake looked correct.
- `components/ch422g` — the minimal CH422G IO-expander driver, shared by both
  boards (same chip, same 0x24/0x38 addresses).
- `components/ui_common` — theme (`ui_theme`), shared dimmer-button widget
  (`ui_dimmer_button`), panel-config types (`panel_def.h`). Buttons show
  label text only (no icon glyph); background swaps dark blue -> light blue
  between off/on (see *UI* section below).
- `components/espnow_link` — ESP-NOW transport for a remote panel with no
  CAN wiring (see *ESP-NOW remote-panel bridge* below). No dependency on
  `main/`; mirrors `dimmer_cmd_msg_t`/`dimmer_status_msg_t` as its own
  ESP-free-of-`main` structs.
- `components/jbd_bms` — Xiaoxiang/JBD Smart BMS protocol codec
  (`jbd_bms_protocol.c`, pure C, host-testable like `rvc_protocol`) plus a
  Bluedroid GATT-client (`jbd_bms_client.c`) for up to 3 fixed battery
  peripherals (see *Battery monitor* below). The **client** runs on the
  basement proxy, not on a panel; a panel links the component only for
  `jbd_bms_protocol.h`'s types and `jbd_bms_combine()`.
- `components/ble_host` — shared Bluedroid bring-up + GAP/GATTC callback
  fan-out, so the proxy can run the battery client and the Power Watchdog
  client at once. Bluedroid is single-tenant in three places and every one
  of them fails **silently**: the stack init calls are one-shot, the GAP and
  GATTC callback registrations hold exactly one function pointer each (a
  second registration *replaces* the first rather than erroring), and GATTC
  app IDs are one flat namespace per node. `ble_host.h` owns the first two
  and records the app-ID allocation for the third. Any new BLE client goes
  through it — never call `esp_ble_gap_register_callback()` /
  `esp_ble_gattc_register_callback()` directly.
  As of issue #51 it also **arbitrates the single GAP scan**: there is one
  scan per node, and both the Watchdog and the solar controller discover by
  advertised name. Clients register a matcher
  (`ble_host_scan_add_matcher()`) plus a found-callback and ask for scanning
  with `ble_host_scan_want()`; nobody calls `esp_ble_gap_start_scanning()`
  directly. The found-callback fires once scanning has actually **stopped**,
  which is what makes it safe to open a connection from it.
- `components/renogy_solar` — Renogy MPPT charge controller (issue #52).
  Same pure-C-codec / ESP-client split as `jbd_bms`:
  `renogy_solar_protocol.c` is host-testable (`host_test/`) and
  `renogy_solar_client.c` is Bluedroid-only.
  ⚠️ **The controller has no radio.** The BT-1/BT-2 module is a transparent
  BLE-to-RS485 bridge, so this speaks plain **Modbus RTU** — not a vendor
  frame format like JBD's or the Watchdog's. Don't go looking for one.
  ⚠️ **Name matching is EXACT, not a prefix** (the opposite of
  `hughes_wd_name_matches()`, and deliberately so). A neighbouring rig's
  Renogy module advertises the same `BT-TH-` prefix, and connecting to it
  gives a perfectly healthy-looking link reporting somebody else's solar.
  Trailing whitespace is ignored because the module pads its advertised name
  to a fixed width (this coach's sends four trailing spaces). An empty name
  falls back to prefix matching, for first bring-up only.
  ⚠️ **A wrong Modbus device id produces SILENCE, not an error**, so the
  client probes the known candidates (255 stand-alone, 16/17 daisy-chained,
  96/97 Communication-Hub) and logs whichever answers. Pin it afterwards.
  Bench-verified on the coach 2026-08-28: the proxy holds **five** BLE links
  at once (3 packs + Watchdog + solar).

## ESP-NOW remote-panel bridge

**Naming convention:** a panel whose ID ends in `_remote` is an **ESP-NOW
device** (no RV-C CAN wiring) that reports to the Mid Coach bridge. A panel
without `_remote` in its ID is **hardwired to the RV-C CAN bus**. `mid_coach`
is the **ESP-NOW router/bridge** between the RV-C bus and all remotes
(`PANEL_IS_BRIDGE 1`).

⚠️ This convention covers **panels only**. The headless nodes — `proxy/` and
the planned `valves/` — are not panels: no display, no LVGL, no
`PANEL_INDEX`, no row in `panels/REGISTRY.md`, and each is its own ESP-IDF
project. They are ESP-NOW participants without being `_remote` anything.

`bedroom_remote` (`panels/bedroom_remote.h`) has no CAN wiring — it
relays button taps to `mid_coach` (`PANEL_IS_BRIDGE 1`) over ESP-NOW, and
`mid_coach` relays real `DC_DIMMER_STATUS_3` changes back to it, using the
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

**Bench-verified 2026-08-13** on real hardware (`mid_coach` on COM16,
`bedroom_remote` on COM11): both boards boot, initialize ESP-NOW, log
the correct peer MAC for each other after pairing, and the full round trip
works — tap on the remote actuates the real load and confirms via the
status echo, and the remote's display updates when the load is toggled from
`mid_coach`'s own button. Known v1 limits, not bugs:
status broadcasts are best-effort with no delivery ack (mitigated by the 30 s
resync, same as RV-C's own status frames having no ack); exactly one
remote/bridge pair; no runtime pairing UI.

**Rename verified 2026-08-16 (issue #23 / PR #24):** the panels were renamed
`living_room` → `mid_coach` and `living_room_remote` → `bedroom_remote`
(filenames, panel IDs, build dirs; on-screen names "MID COACH" / "BED
REMOTE"). `mid_coach` was flashed and **confirmed working on real hardware**
by the user. `bedroom_remote` build is verified but **not yet flashed** —
pending interactive flash + user confirmation.

⚠️ **`sdkconfig` is shared at the repo root across every `-B build_<panel>`
directory** — discovered while pairing the two boards above. Only `PANEL`
(the C source selection) is a per-build-dir CMake cache var; Kconfig
settings like the ESP-NOW peer MAC/PMK/LMK are not, and both panels need
different values. Give each of `mid_coach`/`bedroom_remote` its own
sdkconfig with `-D SDKCONFIG=build_<panel>/sdkconfig` on every `idf.py`
invocation (configure, menuconfig, build, flash) — see
`docs/FLASHING.md` → *ESP-NOW remote panel*. Skipping it on one command
silently edits the shared root `sdkconfig` and the other panel's next build
picks up whatever was last written there.

**Remote panel button layout is independent of `mid_coach`'s own buttons.**
Since `bridge_tx.c`/`state_manager`'s status sink forward whatever instance
they're given regardless of what's in `mid_coach.h`'s `PANEL_BUTTONS[]`,
the remote doesn't need to mirror the bridge's local button set — as of
2026-08-13 `panels/bedroom_remote.h` was reprogrammed to a bedroom/
bathroom-focused layout (instances 17, 25, 35, 46 left column; 18, 13, 21,
Panel Lights right column) unrelated to `mid_coach`'s living-room-focused
buttons. See [docs/instance_map.yaml](docs/instance_map.yaml) for the full
RV-C instance map used to pick these.

## Battery monitor (JBD-BMS via BLE)

**The BLE links live on the basement proxy, not on a panel** (issues
#40–#42, moved there 2026-08-22). The proxy runs a Bluedroid GATT-client
central role (`components/jbd_bms`) against up to 3 Vatrer 300AH batteries'
Xiaoxiang/JBD-BMS boards, and re-broadcasts each pack's reading over ESP-NOW
every poll interval; `bedroom_remote` just displays what arrives. v1 scope
mirrors ESP-NOW's: fixed MAC addresses from Kconfig, no scanning/pairing UI,
no mesh.

Why it moved: the packs are in the same basement bay as the proxy, so
running their links from a bedroom wall panel put a steel bay door in the RF
path and made one panel's radio carry three BLE connections plus ESP-NOW —
for data that any panel would eventually want anyway. Broadcasting it costs
a future panel nothing.

**Bench-verified 2026-08-23** on real hardware: all 3 packs and the Power
Watchdog connect from the proxy simultaneously, `bedroom_remote` shows the
combined bank from broadcasts alone, and both charge and discharge were
observed (which is what finally settled the current-sign question below).
Note the original proxy board died during this work — it powered up and
enumerated its USB bridge but never drove UART TX, not even the boot ROM
banner, in any boot mode at any baud. A replacement classic ESP32 was a
straight swap: the telemetry role has no unicast peer, so no node knows or
cares about the proxy's MAC and nothing needed reconfiguring.

⚠️ **Per-pack frames go on the wire, NOT a pre-combined bank.** The
combining rules already live in `jbd_bms_combine()` as pure host-tested C
and the panel links it regardless, so summing on the proxy as well would
fork that logic across two chip families. Sending packs separately is also
what keeps the panel's per-pack detail popup fed. A configured-but-
unreachable pack is broadcast too, flagged offline — that is how a panel
distinguishes "this pack dropped" from "the whole proxy is gone", which
silence alone cannot express.

**The three packs are wired in PARALLEL, so the screen shows ONE combined
bank reading, not three per-pack gauges** (issues #29–#32): one voltage, one
current, one power figure, one SOC, one time-remaining — the way the coach's
own Vatrer display presents it.

⚠️ **There is no BMS-side aggregation to read instead — don't go looking for
one.** A JBD BLE module is a UART bridge to a single BMS's serial port;
register `0x03` returns only that pack's own data and it knows nothing of its
siblings. Vendor displays that plug into one battery and show the whole bank
are using the packs' **RS485 inter-pack daisy-chain**, a different physical
bus nothing in this project is wired into. The three-BLE-connection
architecture is therefore correct, and the combining happens in
`jbd_bms_combine()` (see below).
Bluedroid rather than NimBLE per explicit project decision — each battery
is its own GATTC "app" (`esp_ble_gattc_app_register()`, app_id == battery
slot index), connected directly by known address (no scan needed, the MAC
is already fixed).

- `CONFIG_FIREFLY_BATTERY_1_MAC`/`_2_MAC`/`_3_MAC` (`main/Kconfig.projbuild`)
  — each battery's BLE MAC. The placeholder `00:00:00:00:00:00` means
  "unconfigured": that slot is never connected to and never counts toward
  the bank's "N of M" indicator. Same shared-`sdkconfig`-per-build-dir
  caveat as the ESP-NOW peer MAC above applies — use
  `-D SDKCONFIG=build_bedroom_remote/sdkconfig`.
- `CONFIG_FIREFLY_BATTERY_POLL_INTERVAL_MS` — **default 30000, range floor
  20000**. JBD BMS units are widely reported to misbehave when polled more
  often than ~every 20 s, so the floor enforces that rather than merely
  documenting it. `jbd_bms_client.c` **derives** its `JBD_HEALTHY_WINDOW_MS`
  staleness window from this (`3 x`) — it was a hardcoded 15000, which
  silently became *shorter* than the poll interval when the default moved to
  30 s, making every pack read as permanently offline between polls. Don't
  re-hardcode it.
  ⚠️ This value is **persisted in each `sdkconfig`**, so changing the Kconfig
  default alone does nothing for an existing build dir — and a stale value
  below the new floor is out of range. Edit the line in place in each
  `sdkconfig`; do **not** regenerate the file (that wipes the real ESP-NOW
  peer MAC and battery MACs — see the gotcha below).
- `components/jbd_bms/jbd_bms_protocol.c` — pure C frame codec (request
  builder, `0x03` "basic info" response parser, NTC temperature decode,
  hours-remaining estimator, `jbd_bms_power_w()`, `jbd_bms_c_to_f()`, and
  the `jbd_bms_combine()` bank aggregator), host-testable exactly like
  `rvc_protocol` (`components/jbd_bms/host_test/test_jbd_bms.c`).

  **Byte layout and checksum scope are now VERIFIED**, not assumed. A real
  published JBD response frame is committed as the regression vector
  `k_jbd_doc_frame` in the host test; its own trailing checksum (`0xFBFF`)
  reproduces exactly under `jbd_checksum()`, which is what confirms the
  documented checksum scope (status + len + payload). Layout:

  | offset | field |
  |--------|-------|
  | 0-1 | total voltage, 0.01 V |
  | 2-3 | current, 0.01 A, signed |
  | 4-5 / 6-7 | residual / full capacity, 0.01 Ah |
  | 8-9 | cycle count |
  | 19 | RSOC (used directly, not recomputed from capacities) |
  | 20 | FET status |
  | 21 | cell/string count |
  | **22** | **NTC probe count** |
  | **23+** | **NTC raw values, uint16 BE; `°C = (raw − 2731)/10`** |

  Cross-checks that pin the offsets: 58.88 V ÷ 15 cells = 3.92 V/cell, and
  the frame's 27-byte payload is exactly 23 + 2×2, i.e. the two probes byte
  22 declares. ⚠️ Some secondary sources put the NTC *count* at offset 21 —
  that frame rules it out (15 probes would need a 53-byte payload). Short
  payloads that stop at the RSOC byte still parse; they just report
  `temp_count == 0` and the UI shows "--".

  **CURRENT SIGN CONVENTION: verified 2026-08-23.** Positive = charging,
  negative = discharging, confirmed on the real packs with both directions
  observed. This sat as an assumption for a long time because the captured
  regression frame carries a zero current field and so cannot settle it —
  only a live pack could. Settled now; don't reopen it without a
  contradicting capture.
- **Watts is not in the protocol** — there is no power field. It is always
  derived. `jbd_bms_combine()` sums per-pack V×I rather than computing
  mean_V × sum_I: identical while the packs agree, but it stays correct if
  one pack reads a different voltage or drops out.
- **Bank aggregation rules** (`jbd_bms_combine()`, pure C, host-tested):
  voltage = mean (packs are hard-tied, so averaging just cancels per-BMS
  shunt/ADC offset); current, power, residual Ah and full Ah = sums; SOC =
  capacity-weighted mean of each BMS's own RSOC (`Σ(soc·full_ah)/Σ(full_ah)`
  — a plain mean for equal packs, but stays right if a pack is replaced with
  a different capacity); time-remaining computed from the **aggregate
  totals**, not averaged from per-pack estimates. Callers pass a **packed
  array of live packs only**, so a pack dropping off BLE shrinks the bank
  rather than dragging the averages toward zero.
- `components/jbd_bms/jbd_bms_client.c` — one connect/discover-service
  (`0xFF00`)/discover-characteristics (notify `0xFF01`, write
  `0xFF02`)/subscribe (register-for-notify + write the CCCD)/poll state
  machine per configured slot, modeled directly on Espressif's Bluedroid
  `gatt_client` example
  (`examples/bluetooth/bluedroid/ble/gatt_client/main/gattc_demo.c`).
  Service/characteristic UUIDs and `BLE_ADDR_TYPE_PUBLIC`
  (`JBD_BMS_ADDR_TYPE` in the file) are **bench-verified 2026-08-20**: all
  3 real batteries connect, subscribe, and stay connected against them —
  no need to try `BLE_ADDR_TYPE_RANDOM`. Polls every
  `CONFIG_FIREFLY_BATTERY_POLL_INTERVAL_MS` once subscribed;
  `jbd_bms_get_status()`/`jbd_bms_healthy()` mirror
  `state_manager_get_tank()`'s valid/invalid contract, and
  `jbd_bms_slot_configured()` separates "configured but offline" from
  "never configured". See *Development notes & gotchas* below for the
  Bluedroid virtual-connection bug that had to be fixed to get all 3
  connecting instead of just the first.

  Now that it runs on the classic ESP32, `try_connect()` picks its connect
  API on `CONFIG_BT_BLE_50_FEATURES_SUPPORTED` — `esp_ble_gattc_enh_open()`
  on the S3, `esp_ble_gattc_open()` on the classic part (the latter is not
  even linked on the S3's default host stack). Stack bring-up and callback
  registration go through `components/ble_host`, and the batteries' GATTC
  app IDs are `BLE_HOST_APP_ID_BATTERY_BASE + slot`, which must stay
  distinct from the Watchdog's.
- **BLE/WiFi coexistence** was bench-verified 2026-08-20 on
  `bedroom_remote` with all 3 BLE connections plus ESP-NOW active, and no
  degradation was observed — which is worth keeping on record even though
  the panel no longer holds those links, because the proxy now carries
  *four* (3 packs + the Watchdog) alongside its ESP-NOW broadcasts. Both
  chips share one 2.4 GHz radio between WiFi and BLE via IDF's software
  coexistence manager (`CONFIG_SW_COEXIST_ENABLE`, on by default once both
  `esp_wifi` and `bt` are enabled).
- UI: broadcasts arrive at `ui_on_battery_status()` (`main/ui/ui.c`), which
  only caches them — mirroring `ui_on_shore_power()`, so exactly one place
  decides what the readout says. `battery_status_timer_cb` then runs once a
  second over that table, packs the fresh packs, calls `jbd_bms_combine()`,
  and pushes the single resulting `jbd_bms_bank_t` to one
  `PANEL_BTN_BATTERY_SUMMARY` widget
  (`components/ui_common/ui_battery_summary.c`). Layout, mirroring the
  Vatrer display: SOC arc with the percent at its center (indicator colored
  by band — green ≥50 %, `UI_COLOR_WARN` 20–49 %, `UI_COLOR_ERR` <20 %), a
  2×2 caption-over-value grid (Total Voltage / Total Power / Total Current /
  time-remaining), and a bottom strip with bank high/low temperature in °F
  and an "N of M" pack indicator that goes amber when a pack is missing.
  The fourth cell's caption tracks direction — "Fully Charged In" /
  "Time Remaining" / "Idle", using the same ±0.05 A idle threshold as
  `jbd_bms_estimate_hours()` so caption and ETA can never disagree — and
  renders as `13h 20m`, not `13.3h`. Current and power are shown as
  magnitudes since the caption already carries direction.
  Tapping the readout toggles a per-pack **detail popup** (MAC, SOC, volts,
  amps, temp, online/offline per slot) for telling which physical battery is
  which during bench troubleshooting; tapping the popup dismisses it.

  **History, so it isn't re-litigated:** this started as three per-pack
  gauges with an animated wave-fill/battery-silhouette graphic; the
  animation was dropped 2026-08-19 for a plain box per user feedback, and
  the three boxes were collapsed into this one combined bank readout
  2026-08-21 (issues #29–#32) because the packs are wired in parallel and
  read as one bank. `ui_battery_gauge.c` and `PANEL_BTN_BATTERY_STATUS`
  were deleted outright rather than left as dead code.
  See *Dual screens* below for how screen 2 picks this layout vs. the tank
  one.

## Basement BLE proxy + broadcast telemetry (issues #33/#34)

`proxy/` is a **second, separate ESP-IDF project** — not a panel, and not
even the same chip. It is a headless **classic ESP32** (ESP32-D0WD-V3, 4 MB
flash, no PSRAM, CP210x USB bridge — auto-resets, no BOOT/RESET button
needed) that sits in the coach's basement bay, holds the BLE link to the
Hughes Power Watchdog, and re-broadcasts readings over ESP-NOW.

```
idf.py -C proxy -B proxy/build set-target esp32      # once
idf.py -C proxy -B proxy/build build
idf.py -C proxy -B proxy/build -p COM4 flash monitor
```

It pulls in shared components **individually** (`EXTRA_COMPONENT_DIRS`
listing `espnow_link`, `hughes_watchdog`, `rvc_protocol`) rather than
pointing at `components/` — the whole directory would drag `board_4_3b` and
`ui_common`, and therefore LVGL, into a build with no display. It also
carries its own `partitions.csv`: the IDF default gives the app 1 MB and a
Bluedroid + WiFi build lands just over that.

**Why a separate node rather than a panel doing it:** the Watchdog accepts
exactly one BLE connection at a time, so whichever device holds it owns it;
and BLE range is the real constraint (the Watchdog is at the shore-power
inlet, panels are on interior walls). Consolidating the three battery packs
onto this node later is an open option — they're in the same bay — but
was deliberately deferred so battery code, which can only be validated at
the coach, wasn't churned before a trip.

### `components/hughes_watchdog` — Gen 1 Power Watchdog

Service `0xFFE0`, notify characteristic `0xFFE2`. **No init command and no
polling** — it streams ~1 Hz once subscribed, and there is no way to slow it
down (so the JBD-style poll-interval concern does not transfer). 40-byte
packets arrive as two 20-byte notifications needing reassembly. A 50 A unit
sends one packet **per line**, tagged at offsets 37-39 (`00 00 00` = L1,
`01 01 01` = L2); a 30 A unit only ever sends L1.

| offset | field |
|--------|-------|
| 0-2 | header `01 03 20` |
| 3-6 / 7-10 / 11-14 / 15-18 | volts / amps / watts / kWh, BE int32 ÷ 10000 |
| 19 | error code (0=OK, 1-9=E1-E9, 11/12=F1/F2) |
| 31-34 | frequency, BE int32 ÷ 100 |
| 37-39 | line ID |

⚠️ **Name matching is a SUBSTRING, not a prefix.** This coach's unit
advertises as **`APMD1CB0DE309`** — leading `A`. Every public integration
prefix-matches `PMD`/`PWS`/`PMS` and would fail to detect it. Don't
"simplify" `hughes_wd_name_matches()` back to a prefix test.

⚠️ **Gen 1 cannot be commanded to switch power, and this is settled.** The
ASCII strings (`relayOn`, `reset`, `setTime`, `backLight`) are known from
the official Android app, but the wire framing is not: writes to `0xfff5`,
`0x1003` and `0x1005`, with and without CRLF, are accepted at the GATT layer
and ignored by the device. TechBlueprints' Gen 1 handler is receive-only for
the same reason. Only **Gen 2** (EPOW models, `WD_V5`/`WD_E5`/`WD_V6`/`WD_E6`,
`$yw@` framing) has a working relay command (`SetOpen`, cmd `0x0B`). The
client here is receive-only by design, not by omission.

⚠️ **Chip portability trap.** `esp_ble_gattc_enh_open()` is the BLE 5.0 path
the S3 needs; the classic ESP32 is BLE 4.2 and wants `esp_ble_gattc_open()`.
Both `hughes_wd_client.c` and `jbd_bms_client.c` select on
`CONFIG_BT_BLE_50_FEATURES_SUPPORTED` in their `try_connect()`. Any new
client on this node needs the same guard.

⚠️ **Two BLE clients share one Bluedroid.** As of issues #40–#42 the
batteries live here too, so `components/ble_host` owns stack bring-up and
the single GAP/GATTC callback slots and fans events out. Both clients also
filter registration events by their own `app_id` — without that, whichever
client registered last would capture the other's `gattc_if` and the two
would silently fight over one connection.

TODO(bench): the byte offsets above are from public reverse engineering, not
from this unit. The client logs the first 5 raw packets at INFO — check them
against the Watchdog's own display before trusting the numbers.

### Broadcast telemetry (`espnow_link`)

Read-only measurements broadcast to every node on the channel, so a panel
can display data it has no connection to. Two producers today: the proxy
(shore power and per-pack battery readings) and the bridge panel (tank
levels, so read-only remotes can show tanks — previously out of scope).

- **Broadcast is unencrypted and unavoidably so** — ESP-NOW cannot encrypt
  broadcast frames. Acceptable *only* because this is read-only telemetry.
  Command/status frames actuate real loads and stay encrypted unicast;
  never move them onto this channel.
- ⚠️ **The control frame's size is part of the wire format.** `espnow_recv_cb`
  validates length, and panels are flashed one at a time — growing
  `espnow_frame_t`'s union would make an updated panel's commands invisible
  to a panel still on the old build, silently. Telemetry is therefore its
  own struct, and a `_Static_assert` pins `sizeof(espnow_frame_t) == 16`.
  If that assert fires, you changed the wire format.
- Scaled integers on the wire, not floats — smaller, and no dependence on
  float layout matching across two chip families.
- `espnow_link_init()` takes an `espnow_role_t` (BRIDGE / REMOTE /
  TELEMETRY) rather than the old `bool is_bridge`. The TELEMETRY role has no
  unicast peer at all and never reads `FIREFLY_ESPNOW_PEER_MAC`.
- Telemetry receipt deliberately does **not** update `espnow_link_healthy()`
  — a broadcast from an unrelated node is not evidence the unicast peer is
  alive.
- The bridge broadcasts the tanks it displays, scanned from `PANEL_BUTTONS_2`
  rather than hardcoded (same trick `ui.c` uses to find GREY/BLACK). Invalid
  readings are broadcast too, so a remote can distinguish "no reading" from
  "0 %". Remote tank telemetry re-enters via `ui_on_tank_status()`, the same
  entry point CAN-fed tanks use.
- Shore power renders two ways on a remote panel: a compact **status bar**
  summary (volts + amps per line) for passive awareness, and a full
  **Line 1 / Line 2 screen** (`PANEL_BTN_SHORE_POWER`,
  `components/ui_common/ui_shore_panel.c`) laid out like the Hughes phone
  app — Volts / Amps / Freq / Watts per column, green digits. Both age out
  to "--" after 20 s of silence, and the staleness decision is made once in
  `shore_power_timer_cb()` so the two can never disagree. A 30 A pedestal
  reports one line, and the Line 2 column is hidden rather than shown as
  zeroes. Note a panel with GREY/BLACK tank buttons already uses the status
  bar's right side for the tank readout — today no panel has both.

⚠️ **All ESP-NOW nodes must share one WiFi channel** (`FIREFLY_ESPNOW_CHANNEL`,
default 1) — there is no AP to negotiate one, and a mismatch is silently
invisible, exactly like a wrong peer MAC.

## Dump-valve node (`valves/`) — designed, NOT built

A **sixth node**: a Waveshare **ESP32-S3-ETH-8DI-8RO** relay board in the
basement bay, driving the two DrainMaster Premium dump valves from the
panels' tank screens. Like `proxy/` it is a **separate ESP-IDF project**,
headless, pulling shared components individually rather than pointing at
`components/` (which would drag LVGL into a build with no display).

**Status: wiring measured and specified, board ordered, no firmware written
and no GitHub issue opened.** Full build spec, wire list and bring-up order:
[docs/DRAINMASTER-VALVES.md](docs/DRAINMASTER-VALVES.md).

⚠️ **Nothing about `proxy/` changes.** The proxy keeps its five BLE links and
its telemetry broadcasts. This is a second board on the same ESP-NOW channel,
deliberately separate: the proxy was already swapped once for running hot,
and a hung proxy today means stale telemetry, whereas a hung node mid-drive
would mean a valve motor left energised.

The facts most likely to be re-derived the hard way:

- ⚠️ **The `-8DO` sibling board is NOT relays.** "8DO" is eight Darlington
  *transistor sinks*, 500 mA, sink-only — it cannot reverse polarity and
  cannot drive the motor. `-8RO` is the relay one. The listing photos are
  identical; only the part number distinguishes them.
- ⚠️ **Relays are behind a TCA9554PWR at I²C `0x20`** (EXIO1–8), not direct
  GPIO. I²C is GPIO41/42, shared with the RTC. DI 1–8 are GPIO4–11.
- ⚠️ **Four relays per valve, NC contacts unwired, all 8 channels consumed.**
  A Form-C NC is connected at rest; using it would park a motor wire at
  ground and short the factory wall rocker when someone pressed it. Unwired
  NC is what makes the rest state a true float, which is the property the
  whole parallel-with-the-rocker design rests on. An earlier two-DPDT design
  was short-proof *by construction*; this one is not, so a **firmware
  interlock refusing "one wire on both rails" is mandatory**.
- ⚠️ **WHITE positive opens** — measured, and the opposite of what the
  DrainMaster harness labels imply. Those labels are wire names, not polarity.
- ⚠️ **The MAG reed senses FULLY CLOSED, not open**, and nothing has an
  end-of-travel cutout. Close is closed-loop; open is a timed run under a
  hard **2 s** ceiling enforced by an independent timer, never a
  `vTaskDelay` inside the drive routine.
- ⚠️ **Do not land the reed pair on a digital input directly.** The DI optos
  want milliamps, which lights DrainMaster's own indicator LED when it should
  be dark. Sense is a 100 kΩ/100 kΩ divider → 2N7000 sinking the DI, 60 µA.

**The real work is ESP-NOW, not valve logic.** `espnow_frame_t` is pinned at
16 bytes by a `_Static_assert`; the link layer allows one fixed unicast peer
per node and this needs a second; and `main_cabinet` — whose TANKS section
hosts the buttons — runs the peerless TELEMETRY role. Broadcast is not
available: it is unencrypted and these frames actuate real loads.

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
mode on `mid_coach`: instance 0 = fresh, 1 = black, 2 = gray (matches
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

Live on `panels/mid_coach.h` ("MID COACH") as of GitHub issues #4/#5:
screen 2 shows FRESH/GREY/BLACK with a BACK button, reached via a
**TANK LEVELS** button in screen 1's bottom-right slot (see *Dual screens*
below). Not yet wired to `ent_center` or `bedroom_remote` — the latter
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

`mid_coach` (on-screen "MID COACH") and `ent_center` were realigned
2026-08-15 to drive the same instance set. **ENTRY CEILING (24)** and
**SECURITY P+H (44, 45 — Note A)** are no longer wired to any CAN-connected
panel (they were dropped from `mid_coach.h`'s button grid); both remain
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
`mid_coach`'s. On a panel with `PANEL_HAS_SCREEN_2` and GREY/BLACK tank
buttons (today: only `mid_coach`), that space now shows the Grey/Black
tank status readout described below (issue #9); other panels' headers are
just the name.

**Grey/Black tank status readout + critical backlight override (GitHub
issue #9, `mid_coach`/"MID COACH" only).** `build_screen()` in
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
color scheme on `mid_coach`/"MID COACH"):** label text only, no icon glyph — the
`panel_btn_def_t.symbol` field was removed along with it, so `panels/*.h`
button rows no longer take an `LV_SYMBOL_*` argument (see any panel header
for the current field order). Label font bumped from Montserrat 16 to 20.
Background is the on/off indicator: dark blue (`UI_COLOR_CARD`, off) to
light blue (`UI_COLOR_CARD_ON`, on), set on the button object itself in
`ui_dimmer_button.c`'s `refresh_visuals()`. Name-label text color still
flips (`UI_COLOR_TEXT_DIM` off, `UI_COLOR_TEXT_ON_LIT` — dark navy — on) for
contrast against the light-blue on-state. The dimmer level bar keeps its own
amber (`UI_COLOR_AMBER`) fill, independent of this background swap.

**Multiple screens (GitHub issue #4, extended for #37).** A panel opts in
with `#define PANEL_HAS_SCREEN_2 1` (default 0, `main/panel_config.h`) plus
a `PANEL_BUTTONS_2[]`/`PANEL_BUTTON_COUNT_2` array, and optionally
`PANEL_HAS_SCREEN_3` + `PANEL_BUTTONS_3[]` (bedroom_remote: battery bank on
2, shore power on 3). `build_screen()` in `main/ui/ui.c` builds **every**
screen up front as sibling containers (all but screen 0 start
`LV_OBJ_FLAG_HIDDEN`) so status updates keep them all correct even while
hidden — switching must never show stale state. `ui.c` holds them as a
`s_screens[]` list rather than named globals, and all the update paths
(`ui_on_status`, `ui_on_tank_status`, shore power) walk that list.

`PANEL_BTN_SCREEN_SWITCH` is local UI nav only, never forwarded as an RV-C
command. **With more than two screens a nav button must say which screen it
targets, via `instances[0]`** (0 = the main grid). A button with
`instance_count == 0` keeps the original toggle-between-0-and-1 behaviour —
that's what lets `mid_coach`'s TANK LEVELS/BACK pair stay untouched.

⚠️ **A widget-update timer must sweep `s_screens[]`, never one screen's
button array.** `battery_status_timer_cb` walked `s_buttons_2[]` /
`PANEL_BUTTON_COUNT_2`, which silently assumed the bank readout lives on
screen 2 — true only while `bedroom_remote` was the only panel with one. Put
the bank on `mid_coach`'s screen 3 (issue #57) and it built fine, showed
`--` forever, and logged nothing. Fixed 2026-08-28; the shore and solar
timers already did this correctly. Every `ui_dimmer_button_update_*()`
ignores buttons of the wrong type, so passing them every button is safe and
is the pattern to copy.

⚠️ **Being `PANEL_IS_BRIDGE` does not grant the proxy's telemetry.** The
role chain in `app_main()` is `#if PANEL_IS_BRIDGE … #elif !PANEL_HAS_CAN …
#elif PANEL_WANTS_TELEMETRY`, so for a long time the bridge never reached
`espnow_link_set_telem_rx_cb()`. Battery/shore/solar reach a bridge the same
way they reach anyone — an ordinary broadcast — and the broadcast peer is
registered for every role, so the frames were arriving with nowhere to go.
Registered for the bridge as of issue #57.

`build_button_grid()` derives its row count from the button count
(`ceil(count/2)`, capped at `GRID_MAX_ROWS`) rather than assuming 4 rows, so
a panel can carry more than 8 entries — `bedroom_remote` has 10 once
BATTERY and SHORE POWER are separate buttons, with a `PANEL_BTN_SPACER`
keeping the two nav buttons together on the bottom row.

Screen 1 stays the plain 2x4 button grid (`build_button_grid()`);
`PANEL_BTN_SPACER` fills a grid cell with nothing, which is how it
positions a button at a specific cell instead of wherever sequential fill
would put it. Screen 2 is read-only, built by `build_screen2_row()` in
`main/ui/ui.c`, and comes in two flavors:

- **tank levels** (`mid_coach`, issues #10/#11) — three
  `PANEL_BTN_TANK_LEVEL` gauges laid out as a centered horizontal row,
  because three tanks really are three separate things;
- **battery bank** (`bedroom_remote`, issues #29–#32) — one
  `PANEL_BTN_BATTERY_SUMMARY` widget filling the area, because the three
  packs are in parallel and read as one bank.

`ui_dimmer_button_create()` dispatches to `ui_tank_wave` or
`ui_battery_summary` by button type, so `ui.c` stays panel-agnostic; the
only layout difference it encodes is the row height (89 % vs 75 % — the
full-width bank card would otherwise leave a dead band above BACK) and
sizing the summary `LV_PCT(100)` instead of a fixed gauge width. The one
`PANEL_BTN_SCREEN_SWITCH` entry ("BACK") is a small button pinned to the
bottom center rather than an equal grid cell — see `panels/mid_coach.h`'s
and `panels/bedroom_remote.h`'s `PANEL_BUTTONS_2[]` (just the gauge
buttons + BACK; no manual spacer positioning needed for this layout).
`build_screen()` decides which optional header widgets/timers to create
(the tank-status text, the battery-status polling timer) by scanning
`PANEL_BUTTONS_2` for the matching button type, so `ui.c` stays
panel-agnostic. Whichever flavor is showing, `idle_timer_cb()` switches
back to screen 1 once the backlight reaches the fully-off idle stage
(300 s) — see *Automatic backlight* below — so a secondary screen is never
left showing after the user walks away; the tank-critical backlight
override (next paragraph) continues to take priority over this.

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

## Side-nav rail (main_cabinet)

`main_cabinet` presents a **persistent left rail** listing its sections
(POWER / TANKS / LIGHTS) with the selected one filling the rest of the
screen, rather than the whole-screen swap the 4.3B panels use. Its 7"
landscape display is what makes room for it; a portrait panel has none to
spare.

Opt in with `PANEL_HAS_NAV_RAIL 1` plus a `PANEL_NAV_RAIL[]` array. Rail
entries are ordinary `PANEL_BTN_SCREEN_SWITCH` defs, reusing the existing
convention that `instances[0]` names the target screen — no new nav type.
`show_screen()` lights the entry whose target matches the visible section,
compared by target rather than by rail position, so **rail order is
independent of screen index**. That is what lets LIGHTS stay screen 0 (where
`build_button_grid()` handles it) while the rail reads POWER / TANKS /
LIGHTS.

Related knobs (`main/panel_config.h`, all defaulted so the other panels are
untouched):

- `PANEL_DEFAULT_SCREEN` — screen shown at boot and returned to when the
  backlight idles off. `main_cabinet` uses 1 (POWER); the grid is just one
  section, not the home screen.
- `PANEL_GRID_COLS` — columns in the main button grid, 2 by default,
  3 on `main_cabinet`.
- `PANEL_WANTS_TELEMETRY` — see the task map above.

Rail screens are laid out by `build_content_pane()` (read-only widgets in a
top row, tappable ones beneath). `build_screen2_row()` is kept as separate
code for the non-rail panels: it lays out screens on three panels that are
flashed and installed, and there is little to gain from merging them.

It did grow its own action row in 2026-08-28 (issue #57), so `mid_coach`'s
tank screen could carry the same dump-valve controls — six items across a
480 px portrait row would squash the gauges to nothing. ⚠️ **That change is
gated on `action_n > 0`, and the gate is the whole safety argument**: every
secondary screen on the installed panels has no action buttons and therefore
takes byte-identical code. When touching this function, keep new behaviour
behind a condition the installed screens cannot satisfy, and verify with
simulator captures of `main_cabinet` TANKS and `bedroom_remote`'s battery
screen rather than by reading the diff.

⚠️ Three latent bugs surfaced while building this and are now fixed — all
of them only bite a CAN panel that shows broadcast telemetry:
the shore-power timer was created only `#if !PANEL_HAS_CAN` (so a CAN panel's
shore readout would render once and never update); the grey/black and battery
scans looked only at `PANEL_BUTTONS_2` (so a panel with tanks on screen 2
got no header readout); and `idle_timer_cb` returned to screen 0 rather than
`PANEL_DEFAULT_SCREEN`. The scans now walk every screen via
`k_screen_defs[]`/`panel_has_button_type()`.

### Two new button types

- `PANEL_BTN_LOCAL_TOGGLE` — shows `label` when off and `label_alt` when
  on, flips on tap, and **sends nothing**. Used for the grey/black dump
  valves and the gravity/macerator selector, whose actuation isn't built
  yet; the control surface exists so the layout is settled when it is.
  State is in memory only. Coloured like any other on-state, *not* with the
  warn colour — an alarm colour on a button that actuates nothing would
  announce an open dump valve that doesn't exist. Revisit when they drive
  real valves.
- `PANEL_BTN_LIGHT_MASTER` — all-lights on/off, tap only, no ramp. See
  below.

`panel_btn_def_t` gained `label_alt` as its **last** field, and every panel
header's button table was converted to **designated initializers** at the
same time. Positional initializers trip `-Wmissing-field-initializers` under
`-Wextra` the moment a field is added; designated ones don't, so the next
field costs no churn at all.

### Light master

**The panel sends exactly what the coach's own rocker sends** (bus-confirmed
2026-08-28, implemented and coach-verified the same day). The factory
"ON / LIGHT MASTER / OFF" rocker (source `0x9F`) sends **six ordinary
`DC_DIMMER_COMMAND_2` frames**, one per group `0x84`–`0x89`, each addressed to
**instance `0xFF`** (all):

- **off** — `FF <grp> 00 06 FF 00 FF FF`, command `0x06` =
  `RVC_DIMMER_CMD_MEMORY_OFF`: off, *remembering* each load's level.
- **on** — `FF <grp> FB 00 FF 00 FF FF`, command `0x00` = SET_LEVEL with
  level `0xFB` (251), the RV-C "restore remembered level" value — not a
  brightness, the normal range is 0–200.

The G6 ACKs each one on `0x0E8FF`, which is **J1939 PGN 59392, the standard
Acknowledgment PGN** — not the "unidentified proprietary DGN" the docs used to
call it. Full capture and byte decode:
[docs/instance_map.yaml](docs/instance_map.yaml) → `light_master`.

`master_apply()` in `main/ui/ui.c` replays those six frames per press. The
groups live in `PANEL_MASTER_GROUPS` (`main/panel_config.h`) because they are
a property of the **coach**, not of any one panel.

- **`RVC_LEVEL_RESTORE` (251) is a SENTINEL, not a brightness.** ⚠️ The
  encoder clamps levels to `RVC_LEVEL_MAX` (200), and that clamp silently ate
  251 the first time this shipped — master-on went out as `SET_LEVEL 200`,
  i.e. "set every light in these groups to 100 %", which lit loads that had
  been off and looked exactly like the old scene behaviour it replaced.
  `rvc_encode_dc_dimmer_command_2()` now exempts the sentinels, and
  `host_test/test_rvc.c` asserts both master frames byte-for-byte against the
  captured ones. Any future sentinel above 200 needs adding to that exemption.
- **Group addressing is plumbed separately from instance addressing.**
  `bridge_enqueue_dimmer_group_cmd()` / `twai_enqueue_dimmer_group_cmd()` set
  instance `0xFF` plus the group byte; `dimmer_cmd_msg_t` carries `group`.
  ⚠️ That struct is **main-local** — `espnow_link` mirrors it with its own,
  whose size is pinned by a `_Static_assert` — so this did not touch the
  ESP-NOW wire format, and must not.
- Displayed state is still "is any light on", refreshed at 1 Hz and **primed
  at build time** — without priming, a tap in the first second after boot saw
  a stale `false` and turned everything on while lights were already on.
- The direction is decided in `panel_send_cb()` from a **fresh** read, not
  from the widget's cached copy, for the same reason.
- `PANEL_HAS_LIGHT_MASTER` (panel header) is checked against `PANEL_HAS_CAN`
  in `panel_config.h`. The preprocessor cannot see inside `PANEL_BUTTONS[]`,
  so the panel declares it. This replaced an older trick of testing for
  `PANEL_MASTER_ON_COUNT`, which only worked while a scene list existed.

⚠️ **Deliberately NOT belt-and-braces with the old state sweep on top.** The
rocker is the reference behaviour, so replaying it is correct by
construction; also sweeping would switch off loads *outside* these groups,
which the factory master does not do.

⚠️ **Pressing ON without a preceding OFF restores whatever the G6 last
remembered**, which may be stale. That is inherent to the MEMORY_OFF/restore
pair and the factory rocker behaves the same way — not a defect to "fix"
without deciding what the alternative should be.

**Bench + coach verified 2026-08-23** (`main_cabinet` on COM19): CAN works
on GPIO20/19 — which also confirms the EXIO5 USB/CAN mux polarity, taken
from ESP3D's notes rather than a schematic — the battery bank and shore
power arrive as ESP-NOW broadcasts, Master on/off behaves as designed
against real loads, and the display geometry and colours are correct,
confirming the RGB timings taken from Espressif's board definition.

**That last open question was answered 2026-08-28**: the rocker does put real
frames on the bus, and the panel now sends them instead of a synthesised
scene — see *Light master* above. Coach-verified the same day, after one
false start (the clamped sentinel described there).

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

## OTA / Wi-Fi update path — removed, USB-only by design (2026-08-16)

Wi-Fi OTA (issues #15–#18) was implemented through a working Wi-Fi STA +
`esp_https_ota` transport (issue #16, PR #20) and then removed. Bench testing
found the target network is 5G-only with WPA3 + PMF required, which the
ESP32-S3's Wi-Fi radio (WPA2-only) cannot associate to — auth fails with
`reason=202` (AUTH_FAIL) despite strong RSSI and correct credentials. This is
a hardware/protocol incompatibility, not a code defect.

**Decision (final): no networked update path at all.** Rather than weaken
network security (e.g. disabling PMF) to work around the radio limitation,
firmware updates are USB-flashed only — see
[docs/FLASHING.md](docs/FLASHING.md). `components/ota_update/` (manifest
parser + transport), the OTA Kconfig menu, `PANEL_HAS_OTA` panel guards, the
dual-OTA partition layout, and bootloader rollback were all removed
accordingly; `partitions.csv` is back to a single `factory` app partition.
Do not reintroduce Wi-Fi/OTA code without revisiting this decision with the
project owner first.

## Conventions / decisions

- ESP-IDF style C; 4-space indent; `s_` prefix for file-static state.
- Protocol logic stays ESP-free in `rvc_protocol` so it remains host-testable.
- New status DGNs get decoded in `twai_rx_task` and flow through the state
  manager — widgets never parse frames.
- Adding a panel = one header in `panels/` + a build flag. No C changes.
  Resolve the A/B question above first; procedure in
  [docs/FLASHING.md](docs/FLASHING.md#adding-a-new-panel).
- Licensed under the MIT License (Copyright (c) 2026 Garrett). Don't add
  code that can't be released that way — no vendored GPL sources, no
  proprietary vendor blobs.
- Decision log: this file, section above. Bench findings (verified pins,
  captured DGNs, instance corrections) should update the tables here and the
  matching TODO comments in code.
- **Development workflow (as of 2026-08-16):** every code change to this
  repo, any size, gets a GitHub issue first (`enhancement` label for new
  capability, `bug` for a defect), scoped to one reviewable piece of work
  — see the issues behind [docs/SPEC-panel-v2.md](docs/SPEC-panel-v2.md)
  or the MID COACH header/tank-wave rework (issues #8–#11, one branch,
  one PR) for examples. Work happens on a branch named/tagged to the
  issue number(s) (`feature/<issue#>[-<issue#>...]-<slug>`, e.g.
  `feature/8-11-tank-header-wave-ui`; multiple issues can share one
  branch when solved together). Implementation gets verified (host tests
  + simulator, build where applicable), **then the actual device is
  flashed and the user is asked to test on real hardware — do not claim
  a fix/feature works, or report success, until the user explicitly
  confirms it on hardware.** Only after that confirmation: commit, open
  a PR that references/closes its issue(s), get CI green, merge the PR
  to `main`, close the issue(s). No direct push to `main`.
  **Exceptions** (no issue/branch/PR/CI needed, safe to edit and commit
  — or just answer — directly): `.md` file updates (README, docs/*.md,
  etc.), memory-file updates (this file and `CLAUDE.local.md`), and
  general questions about the code that don't change anything.

## Repo hygiene — what belongs in git

Two separate ESP-IDF projects live alongside the panel app and follow the
same rules: `proxy/` (built) and `valves/` (planned).

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

⚠️ `components/board_4_3b` was renamed to **`components/board`** (holding
both boards) and the CH422G driver split out into `components/ch422g`. Both
are committed sources like any other component.

## Development notes & gotchas

Hard-won during setup — check here before re-debugging:

- **PowerShell does not expand `$var` in arguments starting with `-`.**
  `cmake -DPANEL=$Panel` passes the literal string `$Panel`. Quote the whole
  argument: `cmake "-DPANEL=$Panel"`. This silently produced a "panel not
  found" error deep in a rebuild, not at configure time.
- **Windows PowerShell 5.1 refuses to source `export.ps1`** ("not digitally
  signed", `UnauthorizedAccess`) — its default execution policy blocks
  unsigned scripts. PowerShell 7 (`pwsh`) runs it fine, which is why this
  only shows up when a command is run by hand in a 5.1 window. Either use
  `pwsh`, or `Set-ExecutionPolicy -Scope Process -Bypass` in that window
  only (session-scoped, reverts on close — don't loosen CurrentUser or
  LocalMachine for this).
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
- ⚠️ **That same 64 KB pool is a hard ceiling on how much UI a panel can
  carry, and overrunning it does not fail cleanly** (2026-08-28).
  `LV_MEM_POOL_EXPAND_SIZE` is 0, so the pool cannot grow; when it fills,
  LVGL's renderer spins on the failed allocation. `main_cabinet`'s four
  screens needed 78 KB to build the UI and peaked at 88 KB rendering — it
  booted anyway (much of the cost is lazy, incurred as each screen is first
  drawn) and then wedged after ~7 nav-rail taps.
  **It presented as three unrelated faults**: `esp_lvgl_port` holds its lock
  across all of `lv_timer_handler()`, so the stuck renderer also blocked the
  ESP-NOW rx task and blanked the battery/shore readouts, while the task
  watchdog only reported `taskLVGL` pegging CPU 1. If a panel ever freezes
  needing a manual reboot, suspect this first.
  `sdkconfig.defaults` now sets `CONFIG_LV_MEM_SIZE_KILOBYTES=128`.
  ⚠️ **IDF applies `sdkconfig.defaults` only when an `sdkconfig` does not yet
  exist**, so every existing `build_<panel>/sdkconfig` must be edited **in
  place** — regenerating one wipes its real ESP-NOW peer MAC and battery
  MACs. As of 2026-08-28 only `main_cabinet` and `bedroom_remote` have been
  raised and reflashed; `mid_coach` and `ent_center` are still on 64 KB.
  Per-device budgets: [docs/SYSTEM.md](docs/SYSTEM.md) → *Memory budget*.
- ⚠️ **The simulator cannot reproduce that class of bug by default**, because
  its allocator is unbounded CLIB. To reproduce one, temporarily set
  `sim/lv_conf.h` to `LV_STDLIB_BUILTIN` with a matching `LV_MEM_SIZE` plus
  `LV_USE_LOG 1` / `LV_LOG_PRINTF 1`, and replay the steps with
  `--shot out.bmp "section:A,B,C,..."` — the comma-separated form is a tap
  **sequence**, which is what reproduces a cumulative failure. Failure shows
  as `lv_realloc: couldn't reallocate memory` then a hang. A
  `lv_mem_monitor()` print per tap separates a leak (growing `used`) from a
  too-large baseline (flat `used`). Revert both files afterwards — they are
  committed sources, not scratch.
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

Hard-won during battery-status BLE bring-up (2026-08-19/20, `bedroom_remote`
on COM11, `mid_coach` on COM16). ⚠️ The BLE client has since moved to the
basement proxy (issues #40–#42), so read "the node running the client" for
`bedroom_remote` below — every one of these still applies, just on a
different board, and the sdkconfig warning now applies to `proxy/sdkconfig`
as well:

- **Regenerating `sdkconfig` wipes anything not in `sdkconfig.defaults`,
  including per-panel secrets like the ESP-NOW peer MAC/PMK/LMK.** Deleting
  the root `sdkconfig` (or a per-panel `build_<panel>/sdkconfig`) to pick up
  a `sdkconfig.defaults` change — done here to switch the BLE host stack —
  silently reset `FIREFLY_ESPNOW_PEER_MAC` back to the placeholder on
  `bedroom_remote`, breaking the already-working ESP-NOW link to
  `mid_coach` with no error on either side (a wrong/placeholder peer MAC
  just means the other board never receives anything). If you must
  regenerate a panel's sdkconfig, re-apply every real MAC/key by hand
  afterward (`idf.py -p COMx read-mac` to recover a board's own address if
  it isn't recorded anywhere) — don't assume Kconfig values survive.
- **`esp_ble_gattc_open()` requires `BT_BLE_42_FEATURES_SUPPORTED`, which is
  off by default on ESP32-S3** (`BT_BLE_50_FEATURES_SUPPORTED` is the
  default host-stack choice instead) — linking against it fails with
  `undefined reference to esp_ble_gattc_open`. Use
  `esp_ble_gattc_enh_open()` with an `esp_ble_gatt_creat_conn_params_t`
  instead (see `jbd_bms_client.c`'s `try_connect()`), the BLE 5.0-
  compatible direct-connect path.
- **Bluedroid delivers `ESP_GATTC_CONNECT_EVT`/`OPEN_EVT`/`DISCONNECT_EVT`
  to every registered GATTC app, not just the one that initiated the
  connection** ("virtual connection" — documented Bluedroid behavior, hit
  here with 3 independent battery apps). Without checking
  `p->connect.remote_bda` (etc.) against the specific slot's own intended
  address, batteries 2 and 3 silently "accepted" battery 1's connection
  the moment it came up, got stuck in `SLOT_CONNECTING` forever (no real
  connection behind it, so discovery never progressed), and — combined
  with the one-connect-attempt-at-a-time serialization
  `jbd_bms_client.c`'s `jbd_bms_task` needs (the BLE controller can only
  have one LE connection attempt in flight; starting a second before the
  first resolves fails outright with `L2CAP - LE - cannot start new
  connection`) — permanently starved every battery after the first from
  ever getting a real connection attempt. Fix: every GATTC event handler
  in `on_gattc_event()` first checks the event's `remote_bda` against
  `slot->bda` and ignores the event entirely if they don't match.

# Spec: automatic backlight, dual screens, tank-sensor display

Status: **draft — not yet approved, no code written.** Per this project's
spec-discipline rule (multi-file feature = spec first), this document
covers goals, interfaces, and a test plan; nothing gets implemented or
flashed until it's reviewed.

Three features, bundled because the third depends on the second:

- **A — Automatic backlight**, replacing the manual PANEL LIGHTS button.
- **B — Dual-screen framework**, a generic mechanism for a panel to have a
  second button grid and a button type to flip between them.
- **C — Tank-sensor display**, the first real use of screen B: a read-only
  widget showing the SeeLevel II 709-RVC's Fresh/Grey/Black percentages
  (protocol already documented in [instance_map.yaml](instance_map.yaml)
  → `tank_dgn`/`tank_sensors`, [CLAUDE.md](../CLAUDE.md) → *Tank sensors*).

---

## A — Automatic backlight

### Current behavior (`main/ui/ui.c`)

- One idle timer, one threshold: 300 s inactive → dim to 20 % via a
  translucent LVGL overlay (`s_dim_overlay`) + `board_backlight_set_percent()`.
- A manual **PANEL LIGHTS** button (`PANEL_BTN_PANEL_LIGHTS`) cycles
  100→60→20→100 % on tap, independent of the idle timer.
- First touch while dimmed is swallowed by the overlay and just wakes the
  screen; it doesn't also trigger whatever's under it.

### Goal

Replace the manual button with two idle stages, both measured from last
touch (`lv_display_get_inactive_time()`, already tracked by LVGL —
no manual reset needed):

| Idle time | Action |
|---|---|
| < 120 s | full brightness (100 %) |
| ≥ 120 s | dim to 20 % (same `IDLE_DIM_PERCENT` as today) |
| ≥ 300 s | backlight fully **off** |

Any touch at any stage restores full brightness immediately and is
swallowed (doesn't also fire the button underneath) — same UX as today's
wake, just with a second stage.

### Why "off" is achievable, not just "very dim"

`board_backlight_set_percent(0)` already drives CH422G EXIO2 (the
backlight enable line) low — it's a real hardware on/off, not emulated.
`apply_backlight()` already handles `percent == 0` correctly (full-opacity
overlay + backlight disabled). So stage 2 is "call `apply_backlight(0)`",
not a new capability.

**Open risk (needs a bench check, see Test plan):** does GT911 touch keep
generating LVGL press events while EXIO2 is low? Touch is I2C, physically
independent of the backlight line, so it should — but this hasn't been
verified with the backlight actually *off* (today it only ever gets dimmed
to 20%, never disabled). If touch stops working with the backlight off,
stage 2 needs a fallback (e.g. a physical wake source, or capping at "very
dim" instead of true off) — **this is the one part of section A I'd want
bench-confirmed before calling it done**, not just built.

### Changes

- `main/ui/ui.c`:
  - Replace `s_auto_dimmed` (bool) with a 3-state enum
    (`BL_NORMAL` / `BL_DIMMED` / `BL_OFF`).
  - Replace `IDLE_DIM_TIMEOUT_MS 300000` with
    `IDLE_DIM_TIMEOUT_MS 120000` and add `IDLE_OFF_TIMEOUT_MS 300000`.
  - `idle_timer_cb()`: NORMAL→DIMMED at 120 s, DIMMED→OFF at 300 s (checked
    against the same `inactive_ms`, not chained/relative timers).
  - `dim_overlay_event_cb()`: wake from either DIMMED or OFF, not just one
    bool state.
  - Remove `s_user_backlight_pct`, `cycle_local_backlight()`, and the
    `PANEL_BTN_PANEL_LIGHTS` branch in `panel_send_cb()` — nothing sends a
    "which brightness" concept anymore, there's only NORMAL/DIMMED/OFF.
  - Drop the `idle_timer_cb` "TEMP DIAGNOSTIC" log line now that idle-dim
    behavior is being rewritten anyway (it was already flagged for removal
    once confirmed).
- `components/ui_common/include/panel_def.h`: remove `PANEL_BTN_PANEL_LIGHTS`
  from `panel_btn_type_t` (see section B — its grid slot becomes the new
  screen-switch button).
- `panels/mid_coach.h`, `panels/ent_center.h`, `panels/bedroom_remote.h`,
  `panels/TEMPLATE.h`: the `PANEL LIGHTS` row in each `PANEL_BUTTONS[]`
  becomes the screen-switch button (see below) — every panel currently
  has one in the same bottom-right grid slot, so the slot doesn't move,
  only what it does changes.

### Non-goals

- No hardware PWM backlight — out of scope (would need a schematic-level
  change; `board_backlight_set_percent()`'s doc comment already flags this
  as a future TODO, unrelated to this spec).
- No user-adjustable brightness setting — always 100/20/0, matching today's
  fixed `IDLE_DIM_PERCENT`.

---

## B — Dual-screen framework

### Goal

A panel can optionally define a second button grid ("screen 2"). One
button type switches between screen 1 and screen 2. Framework only — what
screen 2 actually shows is per-panel (section C is the first real content,
but a panel could just as well put more lights there, per your example).

### Data model

`components/ui_common/include/panel_def.h`:

```c
typedef enum {
    PANEL_BTN_DIMMER,
    PANEL_BTN_SWITCH,
    PANEL_BTN_SCREEN_SWITCH,   /* replaces PANEL_BTN_PANEL_LIGHTS */
    PANEL_BTN_TANK_LEVEL,      /* section C — read-only, no tap action */
} panel_btn_type_t;
```

Panel headers keep declaring `PANEL_BUTTONS[]`/`PANEL_BUTTON_COUNT` for
screen 1 exactly as today. A panel that wants a second screen additionally
declares:

```c
#define PANEL_HAS_SCREEN_2 1        /* default 0, in main/panel_config.h,
                                        same pattern as PANEL_HAS_CAN */
static const panel_btn_def_t PANEL_BUTTONS_2[] = { ... };
#define PANEL_BUTTON_COUNT_2 (sizeof(PANEL_BUTTONS_2) / sizeof(PANEL_BUTTONS_2[0]))
```

Two named arrays (not a generic array-of-N-screens) matches how this
codebase already does optional per-panel features — `PANEL_HAS_CAN`,
`PANEL_IS_BRIDGE` are booleans with a fixed meaning, not a generic plugin
list. Only 2 screens were asked for; building N-screen infrastructure for
a currently-unused case would be the kind of premature generality this
project's conventions call out to avoid.

**Convention:** put the `PANEL_BTN_SCREEN_SWITCH` button in the *same grid
slot* (bottom-right, index 7 — where `PANEL LIGHTS` used to be) on **both**
`PANEL_BUTTONS[]` and `PANEL_BUTTONS_2[]`, so it's always reachable to flip
back. `tools/check_panels.py` gets a new check: if `PANEL_HAS_SCREEN_2` is
set, both arrays must contain exactly one `PANEL_BTN_SCREEN_SWITCH` button
(bench-visible mistake otherwise — a panel with a second screen and no way
back).

### UI changes (`main/ui/ui.c`)

- `build_screen()` builds **both** grids up front (screen 1 always; screen
  2 only if `PANEL_HAS_SCREEN_2`) as sibling `lv_obj_t` containers under
  `scr`, screen 2 created with `LV_OBJ_FLAG_HIDDEN` set. This keeps both
  sets of widgets alive simultaneously so `ui_on_status()` can update
  whichever one is currently hidden — **the invariant that visual state is
  driven only by real status frames must hold even for the screen you're
  not looking at**, otherwise switching back would show stale state.
- `s_buttons[]` becomes `s_buttons[PANEL_BUTTON_COUNT]` (screen 1, as
  today) plus, if `PANEL_HAS_SCREEN_2`, `s_buttons_2[PANEL_BUTTON_COUNT_2]`.
  `ui_on_status()` loops both arrays when screen 2 exists.
- New `switch_screen()`: toggles `LV_OBJ_FLAG_HIDDEN` on the two grid
  containers.
- `panel_send_cb()` gets a new branch: `PANEL_BTN_SCREEN_SWITCH` calls
  `switch_screen()` directly and returns — like the old `PANEL_BTN_PANEL_LIGHTS`
  branch, this never reaches `bridge_enqueue_dimmer_cmd()`, it's pure local
  UI navigation, not an RV-C command.
- `components/ui_common/ui_dimmer_button.c`'s `handle_tap()`: the existing
  `if (def->type == PANEL_BTN_PANEL_LIGHTS) { send(...); return; }`
  early-return becomes `if (def->type == PANEL_BTN_SCREEN_SWITCH) { send(ctx, RVC_DIMMER_CMD_TOGGLE); return; }`
  — same "local action, command value is a don't-care signal" pattern,
  just renamed. No bar, no on/off coloring (the button's `instance_count`
  is 0, so `any_on()` is always false — it just always renders in the
  off/dark-blue state, which reads fine for a nav button).

### Non-goals

- No more than 2 screens.
- No swipe/gesture navigation — button only.
- No per-screen idle/backlight behavior differences — section A's timers
  apply regardless of which screen is showing.

---

## C — Tank-sensor display (first use of screen B)

### Goal

A new read-only button/widget type showing one tank's fill percentage,
fed by `TANK_STATUS` (`0x1FFB7`) frames — fully documented in
[instance_map.yaml](instance_map.yaml) → `tank_dgn`. Intended layout: a
panel's screen 2 with 3 tank widgets (Fresh/Grey/Black) + the
screen-switch button back to screen 1, e.g.:

```
FRESH   |  GREY
--------|--------
BLACK   |  <back to screen 1>
```

(exact layout is a panel-header decision once this is built, same as any
other panel's button grid — not fixed by this spec)

### Data flow

Mirrors the existing `DC_DIMMER_STATUS_3` path in `main/twai_tasks.c`,
as a second decode branch — **not** reusing the dimmer status queue/table,
since tank instance numbers (0/1/2) are a completely different namespace
under a different DGN and must never be conflated with dimmer instance 0.

```
twai_rx_task: id.dgn == RVC_DGN_TANK_STATUS (new: 0x1FFB7)
  -> rvc_decode_tank_status(data, &st)   [new, components/rvc_protocol]
  -> tank_status_msg_t { instance, percent, valid }
  -> new tank status queue (separate from s_status_queue)
       -> state_manager: new small table (fixed-size, not the 256-entry
          dimmer table — only instances 0/1/2/3/16-19 are meaningful here)
       -> ui_on_tank_status(instance, percent, valid) under the LVGL lock
       -> loops s_buttons[]/s_buttons_2[] same as ui_on_status(), updating
          any widget watching that instance
```

`rvc_decode_tank_status()`: `percent = relative_level / resolution`,
clamped 0–100; treat `relative_level == 0xFF` (or `resolution == 0`) as
"not yet reported" (`valid = false`) rather than 0 % — RV-C's usual
`RVC_FIELD_NA` convention, consistent with how this project already treats
`0xFF` fields in the dimmer DGNs. Widget shows `--` instead of `0%` when
`valid` is false, so a boot-time gap before the SeeLevel's first broadcast
doesn't read as "tank empty."

### Widget (`components/ui_common/ui_dimmer_button.c`)

`PANEL_BTN_TANK_LEVEL` buttons:
- No tap/hold behavior — `event_cb()` no-ops `LV_EVENT_SHORT_CLICKED` /
  `LV_EVENT_LONG_PRESSED` for this type (matches how `PANEL_BTN_SWITCH`
  already skips the ramp-only long-press branch).
- Reuses `ctx->bar` (already exists for `PANEL_BTN_DIMMER`) as the level
  fill, always visible (not conditionally hidden by on/off like a dimmer's
  bar) — extend the `if (def->type == PANEL_BTN_DIMMER)` bar-creation
  guard to include `PANEL_BTN_TANK_LEVEL`.
- New `ctx->value_label` under the name label showing `"62%"` / `"--"`.
- Never calls `send()` — it's a pure display widget, `ui_dimmer_button_create()`'s
  `send_cb` argument can be `NULL` for these (already nullable-safe: `send()`
  checks `ctx->send_cb != NULL`).

### `panel_def.h` / panel headers

`panel_btn_def_t.instances[0]` holds the tank instance number (0/1/2);
`instance_count = 1`. No new struct fields needed — reuses the existing
`instances[]`/`instance_count` mechanism, just against a different
DGN/namespace (the button's `.type` is what tells the widget and the
status-routing code which table to consult).

### Non-goals (v1)

- **No ESP-NOW relay of tank data to `bedroom_remote`.** The remote
  panel has no CAN wiring, so it never sees `TANK_STATUS` frames directly;
  bridging them would need a third status-sink type alongside the
  existing dimmer one in `state_manager`. Real feature, deliberately
  deferred — only CAN-connected panels (`mid_coach`, `ent_center`) can
  show tank widgets in v1.
- No LPG (instance 3) — this coach's kit only has Fresh/Grey/Black
  sensors per your message; the DGN supports it but nothing here targets
  it.
- No alarms/thresholds (the SeeLevel unit has its own alarm output,
  separate from this).
- No historical/graphed levels — current value only, like every other
  status-driven widget in this codebase.

---

## Cross-cutting risks / open questions

1. **GT911 touch-while-backlight-off** (section A) — needs a bench check
   before this is considered done, not just built. If touch doesn't wake
   the panel, stage 2 needs to change (see section A).
2. **Grey/black tank instance ordering is unverified** (documented in
   `instance_map.yaml` already) — sniff the bus before wiring the tank
   widget to a real panel meant for daily use, not just the simulator.
3. **`resolution` value is unconfirmed for this specific SeeLevel unit** —
   the `percent = relative_level / resolution` formula is correct per the
   RV-C spec family, but the actual resolution byte this unit sends
   (commonly 2, per the dimmer DGN's own 0.5%-step convention) hasn't been
   captured from this coach's bus yet.
4. Two screens built simultaneously roughly doubles peak widget count (up
   to 16 buttons' worth of LVGL objects instead of 8) — not a real concern
   given the flash/RAM headroom already established for this board, just
   noting it's a real memory delta, not free.

## Test plan

- **Host tests** (`components/rvc_protocol/host_test`): add
  `rvc_decode_tank_status()` coverage — valid frame, `0xFF` "not available"
  sentinel, boundary percent values (0 %, 100 %, resolution edge cases) —
  same pattern as the existing dimmer decode tests.
- **Simulator** (`sim/`): extend `sim_stubs.c` to also echo a synthetic
  `TANK_STATUS` frame (fixed or slowly-varying percentages) so both new
  screens and the tank widget are visually checkable without hardware,
  same as the existing dimmer echo stub. Screenshot both screens of
  whichever panel gets the feature first, same as the MID COACH restyle
  screenshot from the last change.
- **Bench, before any coach install:**
  - Stopwatch-verify the 120 s dim / 300 s off thresholds.
  - Confirm touch wakes the panel from the OFF stage specifically (risk 1).
  - Toggle a light on screen 1, switch to screen 2, switch back — confirm
    the button still reflects the real state (validates that status
    updates apply to the hidden screen too, not just the visible one).
  - With a real (or captured/replayed) `TANK_STATUS` frame, confirm the
    percent shown matches the SeeLevel's own physical display.
- **Sniffer, before wiring the tank widget into a panel meant for real
  use:** capture actual `TANK_STATUS` frames from this coach's SeeLevel
  unit and confirm source address, instance numbers, and resolution byte
  against what's assumed above (risks 2 and 3).
- `python tools/check_panels.py` — extend with the screen-switch-button
  pairing check described in section B, run on every panel after this
  lands.

## What this spec does NOT include

- No implementation — this is the document to review before any of it
  gets written.
- No decision yet on **which panel** gets the tank widget first, or its
  exact screen-2 layout — that's a `panels/<name>.h` edit once this
  framework exists, same as any other button-grid change.
- No changes to `bedroom_remote`'s ESP-NOW bridging (see Non-goals,
  section C).

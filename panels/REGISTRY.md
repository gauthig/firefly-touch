# Panel registry — allocated indices and RV-C source addresses

**This file is the allocation record for `PANEL_INDEX`.** Every panel on the
coach's RV-C bus must have a unique index, because the source address is
derived from it as `0x80 + PANEL_INDEX` (`main/panel_config.h`). Two panels
sharing a source address causes CAN arbitration faults that are miserable to
diagnose from symptoms — the panels appear to work, then intermittently miss
or corrupt frames.

`tools/check_panels.py` validates this file against `panels/*.h` on every CI
run. If you add a panel header without adding a row here, CI fails.

## Allocated

| `PANEL` value | Index | Source addr | Board | On-screen name | Location / replaces |
|---|---|---|---|---|---|
| `mid_coach` | 0 | `0x80` | `4_3b` | MID COACH | Mid coach wall — Entegra SW2-E8 (p/n 0291135 / 75570); ESP-NOW bridge/router to the remotes |
| `ent_center` | 1 | `0x81` | `4_3b` | ENT CENTER | Entertainment center — Entegra SW4-E1 (p/n 0291136 / 75571) |
| `bedroom_remote` | 2 | `0x82`† | `4_3b` | BED REMOTE | No CAN wiring — relays to `mid_coach` over ESP-NOW, see below |
| `main_cabinet` | 3 | `0x83` | `lcd7` | MAIN CABINET | Main cabinet — Waveshare 7" landscape, side-nav rail (Power / Tanks / Lights) |

**Next free index: 4** (source address `0x84`).

† `bedroom_remote` has `PANEL_HAS_CAN 0` (`panels/bedroom_remote.h`)
— it never transmits on the CAN bus, so `0x82` is never actually claimed as
a source address. The index is still allocated from this table because it
doubles as the panel's ESP-NOW peer identity; one allocation table stays
authoritative for every panel regardless of role.

## Boards

The **Board** column names the display-board component
(`components/board_<board>`), and must match the `PANEL_BOARD_<panel>`
mapping in the root `CMakeLists.txt`; panels absent from that mapping get
`4_3b`. `tools/check_panels.py` checks the two agree.

Board is derived from the panel rather than passed on the command line
because the mismatch fails **silently**: the Waveshare 7" puts CAN on
GPIO20/19, where the 4.3B has RS485, so a panel built for the wrong board
boots, lights up, and never sees the bus.

## Naming convention

- A panel whose `PANEL` value ends in **`_remote`** is an **ESP-NOW device**:
  no RV-C CAN wiring. It reports to the Mid Coach bridge.
- A panel **without** `_remote` in its `PANEL` value is **hardwired to the
  RV-C CAN bus**.
- `mid_coach` is the **ESP-NOW router/bridge** between the RV-C bus and all
  remote panels (`PANEL_IS_BRIDGE 1`).

### Non-panel nodes are not allocated here

Headless nodes have no display, no `PANEL_INDEX` and no RV-C source address,
so they get no row in this table and `tools/check_panels.py` never sees them.
Each is its own ESP-IDF project:

| Node | Project | Hardware | Talks |
|---|---|---|---|
| Bluetooth proxy basement | `proxy/` | ESP32-D0WD-V3 (classic) | BLE ×5, ESP-NOW broadcast |
| Dump-valve controller *(planned)* | `valves/` | Waveshare ESP32-S3-ETH-8DI-8RO | ESP-NOW unicast |

They are still ESP-NOW participants and must share
`FIREFLY_ESPNOW_CHANNEL` with every panel. See
[../docs/SYSTEM.md](../docs/SYSTEM.md) and
[../docs/DRAINMASTER-VALVES.md](../docs/DRAINMASTER-VALVES.md).

## Rules

1. **Updating an existing panel** (new buttons, renamed labels, changed
   instances): keep its current index and source address. Editing the existing
   header is the whole change — this file does not need a new row.
2. **Adding a new panel**: take the next free index above, add a row here, and
   copy `panels/TEMPLATE.h` to `panels/<name>.h`.
3. Never reuse or renumber an index of a panel that has been flashed and
   installed, even if it is temporarily removed from the coach.
4. Indices must stay within the RV-C user address range. `0x80`–`0xF9` is
   available, so indices 0–121 are usable — practical limit is the coach.

> ⚠️ Addresses here are **assumed free**, not verified. Before putting the
> first panel on the live bus, run a sniffer-mode build and confirm nothing
> already transmits from `0x80`+. See [../docs/FLASHING.md](../docs/FLASHING.md).

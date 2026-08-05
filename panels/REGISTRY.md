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

| `PANEL` value | Index | Source addr | On-screen name | Location / replaces |
|---|---|---|---|---|
| `living_room` | 0 | `0x80` | LIVING ROOM | Living room wall — Entegra SW2-E8 (p/n 0291135 / 75570) |
| `ent_center` | 1 | `0x81` | ENT CENTER | Entertainment center — Entegra SW4-E1 (p/n 0291136 / 75571) |

**Next free index: 2** (source address `0x82`).

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

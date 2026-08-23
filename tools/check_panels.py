#!/usr/bin/env python3
"""
Validate panel definitions against the registry.

Every panel on the RV-C bus needs a unique PANEL_INDEX, because its source
address is 0x80 + PANEL_INDEX. Duplicates cause CAN arbitration faults that
present as intermittent dropped frames rather than an obvious failure, so this
is checked mechanically instead of by eye.

Checks:
  1. Every panels/*.h defines PANEL_NAME and PANEL_INDEX.
  2. No two panels share a PANEL_INDEX.
  3. Every panel header has a row in panels/REGISTRY.md with a matching index.
  4. Every registry row has a corresponding header (no stale rows).
  5. The registry's "Next free index" is actually free and correct.
  6. Each row's Board column matches the PANEL_BOARD_<panel> mapping in the
     root CMakeLists.txt (panels absent from that mapping default to 4_3b),
     and that components/board/board_<board>.c actually exists. A wrong board is worse than a
     wrong index: the Waveshare 7" puts CAN where the 4.3B puts RS485, so
     the panel boots and looks fine with a permanently silent bus.

Usage:
    python tools/check_panels.py            # human-readable report
    python tools/check_panels.py --matrix   # JSON panel list, for CI matrices
"""

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PANEL_DIR = REPO / "panels"
REGISTRY = PANEL_DIR / "REGISTRY.md"
ROOT_CMAKE = REPO / "CMakeLists.txt"

DEFAULT_BOARD = "4_3b"

# Not a real panel: it is the copy-me starting point and intentionally
# contains a placeholder index plus an #error guard.
SKIP = {"TEMPLATE.h"}

RE_NAME = re.compile(r'^\s*#define\s+PANEL_NAME\s+"([^"]*)"', re.MULTILINE)
RE_INDEX = re.compile(r"^\s*#define\s+PANEL_INDEX\s+(\d+)", re.MULTILINE)
RE_ROW = re.compile(r"^\|\s*`([A-Za-z0-9_]+)`\s*\|\s*(\d+)\s*\|", re.MULTILINE)
# panel | index | source addr | board | ...
RE_ROW_BOARD = re.compile(
    r"^\|\s*`([A-Za-z0-9_]+)`\s*\|\s*\d+\s*\|[^|]*\|\s*`([A-Za-z0-9_.]+)`\s*\|",
    re.MULTILINE,
)
RE_NEXT = re.compile(r"\*\*Next free index:\s*(\d+)\*\*")
RE_CMAKE_BOARD = re.compile(
    r'^\s*set\(PANEL_BOARD_([A-Za-z0-9_]+)\s+"([A-Za-z0-9_.]+)"\)', re.MULTILINE
)


def load_panels():
    """Return {panel_name: (index, on_screen_name)} from panels/*.h."""
    panels, errors = {}, []
    for path in sorted(PANEL_DIR.glob("*.h")):
        if path.name in SKIP:
            continue
        text = path.read_text(encoding="utf-8")
        name, index = RE_NAME.search(text), RE_INDEX.search(text)
        if not index:
            errors.append(f"{path.name}: no #define PANEL_INDEX")
            continue
        if not name:
            errors.append(f"{path.name}: no #define PANEL_NAME")
            continue
        panels[path.stem] = (int(index.group(1)), name.group(1))
    return panels, errors


def load_registry():
    """Return ({panel: index}, {panel: board}, next_free_index, errors)."""
    if not REGISTRY.exists():
        return {}, {}, None, [f"missing {REGISTRY.relative_to(REPO)}"]
    text = REGISTRY.read_text(encoding="utf-8")
    rows = {m.group(1): int(m.group(2)) for m in RE_ROW.finditer(text)}
    boards = {m.group(1): m.group(2) for m in RE_ROW_BOARD.finditer(text)}
    nxt = RE_NEXT.search(text)
    return rows, boards, (int(nxt.group(1)) if nxt else None), []


def load_cmake_boards():
    """Return {panel: board} from the root CMakeLists.txt PANEL_BOARD_* map."""
    if not ROOT_CMAKE.exists():
        return {}, [f"missing {ROOT_CMAKE.relative_to(REPO)}"]
    text = ROOT_CMAKE.read_text(encoding="utf-8")
    return {m.group(1): m.group(2) for m in RE_CMAKE_BOARD.finditer(text)}, []


def main():
    panels, errors = load_panels()
    registry, reg_boards, next_free, reg_errors = load_registry()
    errors += reg_errors
    cmake_boards, cmake_errors = load_cmake_boards()
    errors += cmake_errors

    def board_of(panel):
        return cmake_boards.get(panel, DEFAULT_BOARD)

    # 2. duplicate indices among actual panel headers
    by_index = {}
    for panel, (index, _) in panels.items():
        by_index.setdefault(index, []).append(panel)
    for index, owners in sorted(by_index.items()):
        if len(owners) > 1:
            errors.append(
                f"PANEL_INDEX {index} (source addr 0x{0x80 + index:02X}) used by "
                f"{', '.join(sorted(owners))} — indices must be unique"
            )

    # 3/4. header <-> registry agreement
    for panel, (index, _) in sorted(panels.items()):
        if panel not in registry:
            errors.append(
                f"'{panel}' has no row in panels/REGISTRY.md — add it "
                f"(index {index}, source addr 0x{0x80 + index:02X})"
            )
        elif registry[panel] != index:
            errors.append(
                f"'{panel}': header says index {index}, "
                f"REGISTRY.md says {registry[panel]}"
            )
    for panel in sorted(registry):
        if panel not in panels:
            errors.append(
                f"REGISTRY.md lists '{panel}' but panels/{panel}.h does not exist"
            )

    # 6. board agreement between the registry and the build's PANEL->BOARD map
    for panel in sorted(panels):
        want = board_of(panel)
        if panel not in reg_boards:
            errors.append(
                f"'{panel}': REGISTRY.md row has no Board column "
                f"(expected `{want}`)"
            )
        elif reg_boards[panel] != want:
            errors.append(
                f"'{panel}': REGISTRY.md says board `{reg_boards[panel]}`, "
                f"CMakeLists.txt maps it to `{want}`"
            )
        if not (REPO / "components" / "board" / f"board_{want}.c").is_file():
            errors.append(
                f"'{panel}': board `{want}` has no "
                f"components/board/board_{want}.c"
            )
    for panel in sorted(cmake_boards):
        if panel not in panels:
            errors.append(
                f"CMakeLists.txt maps board for '{panel}', "
                f"but panels/{panel}.h does not exist"
            )

    # 5. next-free bookkeeping
    if next_free is None:
        errors.append("REGISTRY.md has no '**Next free index: N**' line")
    elif panels:
        expected = max(i for i, _ in panels.values()) + 1
        if next_free in by_index:
            errors.append(
                f"REGISTRY.md says next free index is {next_free}, "
                f"but it is already used by {', '.join(by_index[next_free])}"
            )
        elif next_free != expected:
            errors.append(
                f"REGISTRY.md says next free index is {next_free}, expected {expected}"
            )

    if "--matrix" in sys.argv:
        # CI consumes this to build one job per panel automatically.
        if errors:
            print("panel validation failed; run without --matrix", file=sys.stderr)
            return 1
        print(json.dumps(sorted(panels)))
        return 0

    print(f"{len(panels)} panel(s) defined in panels/\n")
    for panel, (index, screen) in sorted(panels.items(), key=lambda kv: kv[1][0]):
        print(f"  {panel:<16} index {index:<3} source addr 0x{0x80 + index:02X}"
              f"   board {board_of(panel):<6} \"{screen}\"")
    if next_free is not None:
        print(f"\n  next free      index {next_free:<3} "
              f"source addr 0x{0x80 + next_free:02X}")

    if errors:
        print(f"\nFAILED — {len(errors)} problem(s):", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print("\nOK — indices unique, boards mapped, registry in sync.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

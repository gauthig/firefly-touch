# RV-C bus survey and custom-DGN probe

Diagnostic build of `mid_coach` (issue #96). It answers two questions before any of the
climate / battery / shore / solar data is moved onto the RV-C bus:

1. **What is really on this coach's bus?** Which DGNs, instances and source addresses, how
   often, and what changes.
2. **Does the Firefly G6 ignore DGNs it doesn't know?** The probe transmits synthetic frames in a
   block of candidate custom DGNs while the survey watches for any side effect.

Nothing here is compiled into a production build: both features are Kconfig options that
default off.

## Building it

The survey image is built from a **copy** of `mid_coach`'s sdkconfig so the real peer MACs and
keys are never regenerated:

```powershell
$env:Path = "C:\Program Files\Python314;C:\Program Files\Python314\Scripts;" + $env:Path
. C:\esp\esp-idf\export.ps1
New-Item -ItemType Directory -Force build_mid_coach_survey | Out-Null
Copy-Item build_mid_coach\sdkconfig build_mid_coach_survey\sdkconfig
Add-Content build_mid_coach_survey\sdkconfig "`nCONFIG_FIREFLY_BUS_SURVEY=y`nCONFIG_FIREFLY_BUS_PROBE=y"
idf.py -B build_mid_coach_survey "-DPANEL=mid_coach" "-DSDKCONFIG=build_mid_coach_survey/sdkconfig" build
```

Flash it over the USB port (COM16) as usual. **To restore**, reflash the normal
`build_mid_coach` image and confirm the boot log has no `survey:` lines.

## Capturing

Either port works for both the log **and the commands**: the UART (CH343) port through stdin, and
the USB (COM16) port by polling the USB-Serial/JTAG receive FIFO directly. (The IDF console itself is
output-only over USB, which is why this build reads the FIFO itself.) One cable is enough.

```powershell
pwsh tools\survey_capture.ps1 -Port COM16 -Log survey.log -ResetOnStart   # logs from boot
Set-Content survey.cmd 't'                                                # from a second shell: send a command
```

`-ResetOnStart` sends one EN pulse with DTR held low so the log starts at the boot banner. DTR and RTS
are otherwise held low: asserting them resets the chip, sometimes into ROM download mode.
Without any input the survey is still useful: every `CONFIG_FIREFLY_BUS_SURVEY_TABLE_S` seconds (300 by
default) it prints the source list and the full table on its own.

## Console commands

| key | does |
|---|---|
| `h` | help |
| `t` | dump the whole table (one line per DGN / source / instance) |
| `a` | per-source-address summary |
| `b` | freeze the current counts as the **baseline** |
| `d` | rate deltas since the baseline (>30 % change, or any new key) |
| `c` | clear the survey and start over |
| `p` | probe status and a free / SEEN check of every DGN in the block |
| `1` | probe **stage 1**: one DGN at 1 frame/s |
| `2` | probe **stage 2**: all 16 DGNs at their planned rates, about 5 frames/s |
| `x` | **stop the probe immediately** |

## Output lines

`SUM` (every 60 s): frames, fps, peak fps, estimated bus load, keys, TWAI state and error
counters. `NEW dgn sa b0 ...` for every new key. `CLAIM` and `REQUEST` for address-claim and request
traffic, `DIAG` when a diagnostic group changes. `T ...` table rows, `SA ...` source rows, `D ...` delta rows.

`0FECA` is the standard J1939 **DM1** heartbeat that every device on this coach sends (the G6, the
wall switch panels, the tank sensor); `1FECA` is the RV-C DM_RV variant. Both count as diagnostics here:
a probe stage aborts if either appears new or changes while it is running.

Bus load is an estimate (nominal bits plus an average stuffing allowance). `mid_coach` cannot hear
its own transmissions, so probe frames never appear in the table; their effect is judged from
everything else and from the TWAI counters.

## Probe safety rules (all enforced in firmware)

- Idle at boot; starts only on `1` or `2`.
- Refused until the survey has run `CONFIG_FIREFLY_PROBE_MIN_SURVEY_S` (300 s default).
- Refused if **any** DGN in the block has ever been seen from any source.
- Sends from `mid_coach`'s normal source address at priority 6, at most one frame per 100 ms.
- Aborts itself on: TWAI not running, 3 TX failures, 10 bus errors, TX queue stuck, a probe DGN
  appearing from another source, or any **new or changed diagnostic** (DM1 / DM_RV) frame.
- `x` is handled by its own task, so a long table dump cannot delay it.

The G6 once wedged on mis-numbered ramp commands until power-cycled. New DGNs are a different
risk, but run the probe only with someone watching the Firefly displays and app.

## What the probe sends

The block starts at `CONFIG_FIREFLY_PROBE_DGN_BASE` (default `0x0FFC0`, J1939 proprietary-B space) and
covers the **16 DGNs** of the custom family in [RVC-CUSTOM-DGNS.md](RVC-CUSTOM-DGNS.md) (offsets 0–F).

- **Stage 1:** offset 0 only, one frame per second.
- **Stage 2:** all 16, at the rates the spec plans (HVAC status 2 s per zone, shore lines 2 s, packs,
  banks, solar and heartbeat 5 s, extras/detail/pedestal 10 s; valve status and both command DGNs once per
  10 s). That is 52 frames per 10 s cycle, about 5 frames/s, placed so no 100 ms tick ever carries two.

Probe frames carry `instance, counter, 0x46 0x54 ("FT"), stage, FF FF FF` so they are obviously
synthetic. The command DGNs (`0FFCA` HVAC command, `0FFCE` valve command) use instance `0xFF` ("not
available"), so nothing that later learns to act on those DGNs could take a probe frame for a command.

Until step 0 of #98 (issue #99), the probe sent only offsets 0–7 at earlier placeholder rates.

## Results

First coach session, `mid_coach` survey build over USB (COM16), 2026-11-22. Hardware-confirmed by the owner:
no displays, errors or lights misbehaved.

**The bus (15 min survey, 14 keys, 6 source addresses).** About 9 frames/s, peak 15, estimated load 0.5 %.
No TWAI errors. No address claims or requests, so no J1939 address claiming is in use.

| DGN | source | what | period |
|---|---|---|---|
| `1FEDA` | `8F` (G6) | dimmer status, one key per ON load (4 on at the time) | 2.0 s |
| `0FECA` | `8F`, `9C`–`9F`, `AF` | J1939 DM1 diagnostic heartbeat, payload constant | 1.0 s (`AF` irregular) |
| `1FFB7` | `AF` | tank status, instances 0/1/2 | about 1.85 s |
| `1FFDC` | `9C` | unidentified, one key | 5.0 s |

The tank sensor is source `AF` on this coach, not `0x48` as `CLAUDE.md` says.
The G6 only broadcasts status for loads that are ON.

**Probe.** Block `0FFC0`–`0FFC9` checked free; frames actually sent were offsets 0–7 (`0FFC0`–`0FFC7`),
from source `0x80` at priority 6.

| stage | duration | frames sent | TX failures | rate deltas vs baseline | diagnostics / new keys |
|---|---|---|---|---|---|
| 1 (1 frame/s) | 168 s | 168 | 0 | 0 | none |
| 2 (full family, ~10 frames/s) | about 12 min | 7,282 | 0 | 0 | none |

The only counter that moved was `arb_lost`, by 2 (normal when two nodes start together; hardware retries).
**The G6 and the other devices ignored the unknown DGNs.** Not covered in that session: transmitting
`0FFC8`–`0FFCF` (closed by the second session below) and a multi-hour soak.

### Second session: all 16 DGNs (2026-11-25, issue #99, step 0 of #98)

Same panel and port. The probe now sends the whole block at the rates the spec plans.

| | result |
|---|---|
| Survey before the probe | 318 s, 12 keys (two G6 dimmer keys fewer than last time: fewer lights on), ~8 frames/s, 0.5 % est. load, no TWAI errors |
| Free-check | all 16 of `0FFC0`–`0FFCF` free |
| Stage 2 | 704 s, **3,671 frames** (~5.2/s) across all 16 DGNs, 0 TX failures, 0 bus errors |
| Rate deltas vs baseline | **0** (G6 lights 2.0 s, DM1 1.0 s, tanks ~1.85 s, `1FFDC` 5.0 s all unchanged) |
| Diagnostics / claims / requests / new keys | none |

`arb_lost` rose by 4 over the 12 minutes (normal, hardware-retried). The owner watched the Firefly displays and
lights throughout and saw nothing odd. **The whole block is now transmit-tested on this coach.** `mid_coach` was
restored to its normal image afterwards: clean boot, no survey output, ~134 KB internal RAM free after display init.

The custom layouts built on this are in [RVC-CUSTOM-DGNS.md](RVC-CUSTOM-DGNS.md).

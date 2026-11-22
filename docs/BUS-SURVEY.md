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
| `2` | probe **stage 2**: the whole family, about 10 frames/s |
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

## Provisional custom DGN family

Block starts at `CONFIG_FIREFLY_PROBE_DGN_BASE` (default `0x0FFC0`, J1939 proprietary-B space).
**Provisional**: layouts are decided after the survey shows what is actually free. Per the project
direction these are our own parameters for this coach; they are not meant to be adopted by the G6.

| offset | content |
|---|---|
| +0 | HVAC zone status: instance = zone, mode, fan, heat/cool setpoints, ambient |
| +1 | HVAC zone extras: heat source (Aqua-Hot / electric), current mode, per-mode fan |
| +2 +3 +4 | battery pack 1 / 2 / 3: volts, amps, SOC, temperatures, online |
| +5 | battery bank aggregate: volts, amps, watts, SOC, time remaining, N-of-M |
| +6 | shore power: instance = line A / line B, volts, amps, watts, frequency, error |
| +7 | solar controller |
| +8 +9 | spare |

Probe frames carry `instance, counter, 0x46 0x54 ("FT"), stage, FF FF FF` so they are obviously
synthetic.

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
**The G6 and the other devices ignored the unknown DGNs.** Not covered: `0FFCA`–`0FFCF`, a multi-hour
soak, transmitting `0FFC8`–`0FFCF`, and anything only visible in the Firefly displays or app (owner saw nothing).

The custom layouts built on this are in [RVC-CUSTOM-DGNS.md](RVC-CUSTOM-DGNS.md).

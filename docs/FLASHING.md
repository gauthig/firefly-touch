# Flashing guide — which firmware goes on which panel

Every panel runs the **same codebase**. What makes a board "the mid coach
panel" is a single build-time parameter, `PANEL`, which selects a header from
`panels/`. That header decides the panel's on-screen name, its button grid and
RV-C instances, and its **RV-C source address** (a unique bus identity).

> ⚠️ **Read before flashing a board that will be connected to the coach.**
> The TWAI (CAN) TX/RX GPIO pins in `components/board_4_3b/include/board_4_3b.h`
> are **unverified** against the Waveshare 4.3B schematic. Verify them, and
> sniff the bus to confirm no source-address collision, before connecting a
> panel to a live RV-C bus.

## Device identity

**[`panels/REGISTRY.md`](../panels/REGISTRY.md) is the canonical list** of
every panel, its `PANEL_INDEX`, and its RV-C source address. It is not
duplicated here so the two can't drift apart.

The build directory convention is `build_<PANEL>` — one per panel, so the
cached `PANEL` value can never be mismatched.

To see the current allocation at a glance:

```powershell
python tools/check_panels.py
```

```
  mid_coach         index 0   source addr 0x80   "MID COACH"
  ent_center        index 1   source addr 0x81   "ENT CENTER"
  bedroom_remote    index 2   source addr 0x82†  "BED REMOTE"
  next free         index 3   source addr 0x83
```

† `bedroom_remote` has no CAN wiring (`PANEL_HAS_CAN 0`) and never
transmits on the bus — see *ESP-NOW remote panel* below. Its index is still
allocated here because it doubles as the panel's ESP-NOW peer identity.

The source address is derived as `0x80 + PANEL_INDEX` in
`main/panel_config.h`. **Two panels must never share an index** — duplicate
source addresses on a CAN bus cause arbitration faults that show up as
intermittent dropped frames, not an obvious failure. CI runs the check above
on every push and fails on collisions.

## Step 1 — Set up the environment

Once per terminal session:

```powershell
. C:\esp\esp-idf\export.ps1
```

## Step 2 — Identify the target board's serial port

Plug in **one** board at a time over USB-C and find its port:

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' } | Select-Object Name
```

On Linux/macOS the device is typically `/dev/ttyACM0` or `/dev/cu.usbmodem*`.

Because all boards are physically identical, **label each panel's enclosure**
with its `PANEL` name as soon as it is flashed. There is no way to tell them
apart by looking at the hardware.

## Step 3 — Build and flash the correct panel

### Mid coach panel

```powershell
idf.py -B build_mid_coach -DPANEL=mid_coach -p COM5 flash monitor
```

### Entertainment center panel

```powershell
idf.py -B build_ent_center -DPANEL=ent_center -p COM5 flash monitor
```

### Bedroom remote panel (no CAN — see *ESP-NOW remote panel* below)

```powershell
idf.py -B build_bedroom_remote -DPANEL=bedroom_remote -p COM5 flash monitor
```

Replace `COM5` with the port from step 2. Keep the `-B <dir>` and `-DPANEL=`
arguments **together and consistent** — `PANEL` is cached in the build
directory, so mixing them up silently reflashes the wrong panel. Using one
build directory per panel (as above) prevents this.

If you must reuse a build directory for a different panel, clean it first:

```powershell
idf.py -B build_mid_coach fullclean
```

## Step 4 — Verify the board got the right firmware

Watch the boot log (`idf.py monitor`, exit with `Ctrl+]`). The panel announces
its identity in the first lines:

```
I (312) main: firefly-touch panel 'MID COACH' (index 0)
I (534) board_4_3b: display up: 800x480 RGB565, GT911 touch, LVGL on core 1
I (536) main: RV-C source addr 0x80
I (541) board_4_3b: TWAI up at 250 kbps on TX=15 RX=16 (TODO: pins unverified...)
I (549) twai_tasks: RX/TX tasks running on core 0, source addr 0x80
I (551) espnow_link: ESP-NOW link up (bridge), peer XX:XX:XX:XX:XX:XX, channel 1
```

The `espnow_link` line only appears on `mid_coach` (the bridge) and on
`bedroom_remote`; `ent_center` has neither ESP-NOW nor a peer configured
and won't print it.

The panel name is also shown in the top-left of the status bar on screen — the
quickest way to confirm a mounted panel without a serial cable.

## ESP-NOW remote panel

`bedroom_remote` has no CAN wiring at all — it relays button taps to
`mid_coach` (its "bridge") over ESP-NOW, and `mid_coach` relays real
`DC_DIMMER_STATUS_3` changes back to it. See `CLAUDE.md` → *ESP-NOW
remote-panel bridge* and `components/espnow_link/` for the design. v1 is a
single fixed peer pair, configured entirely through Kconfig — no runtime
pairing.

**Naming convention:** any panel whose `PANEL` value ends in `_remote` is an
ESP-NOW device (no RV-C CAN wiring) that reports to the Mid Coach bridge.
Panels without `_remote` in the name are hardwired to the RV-C CAN bus.
`mid_coach` is the ESP-NOW router/bridge between the RV-C bus and all
remotes.

> ⚠️ **`sdkconfig` is shared at the repo root across every `-B build_<panel>`
> directory** — only `PANEL` (the C source selection) is cached per build
> dir; Kconfig settings are not. Since the peer MAC/PMK/LMK below must
> differ between `mid_coach` and `bedroom_remote`, **give each build
> its own sdkconfig file** with `-D SDKCONFIG=build_<panel>/sdkconfig` on
> every `idf.py` invocation for these two panels (configure, menuconfig,
> build, flash — all of it). Skipping this on even one command silently
> edits the shared root `sdkconfig` and the other panel's next build picks
> up whatever was last written there. `ent_center` doesn't need this since
> it never touches ESP-NOW Kconfig.

**Before flashing either side:**

1. Read each board's factory MAC address (needs a build directory that
   already exists, e.g. after step 1's environment setup):

```powershell
idf.py -p COM5 read-mac
```

2. Give each panel its own sdkconfig, then configure the peer relationship —
   each panel's Kconfig points at the *other* panel's MAC:

```powershell
idf.py -B build_mid_coach -DPANEL=mid_coach -D SDKCONFIG=build_mid_coach/sdkconfig menuconfig
idf.py -B build_bedroom_remote -DPANEL=bedroom_remote -D SDKCONFIG=build_bedroom_remote/sdkconfig menuconfig
```

Navigate to *Firefly Touch Panel* → *ESP-NOW remote-panel link* on each and
set:

| Setting | On `mid_coach` | On `bedroom_remote` |
|---|---|---|
| `FIREFLY_ESPNOW_PEER_MAC` | `bedroom_remote`'s MAC | `mid_coach`'s MAC |
| `FIREFLY_ESPNOW_CHANNEL` | same value on both | same value on both |
| `FIREFLY_ESPNOW_PMK` | same value on both | same value on both |
| `FIREFLY_ESPNOW_LMK` | same value on both | same value on both |

**Change `FIREFLY_ESPNOW_PMK`/`FIREFLY_ESPNOW_LMK` from the placeholder
defaults before deploying** — these frames actuate real loads and ESP-NOW's
encryption is only as good as the key. Both are truncated/zero-padded to 16
bytes; keep them at 16 ASCII characters.

3. Build and flash both boards, keeping the same `-D SDKCONFIG=` flag used in
   step 2 (CMake caches it per build dir, so it's optional on later commands
   in the same dir, but pass it explicitly to be safe — same reasoning as
   always repeating `-DPANEL=`):

```powershell
idf.py -B build_mid_coach -DPANEL=mid_coach -D SDKCONFIG=build_mid_coach/sdkconfig -p COM5 flash
idf.py -B build_bedroom_remote -DPANEL=bedroom_remote -D SDKCONFIG=build_bedroom_remote/sdkconfig -p COM5 flash
```

A link-health dot appears in `bedroom_remote`'s status bar, same place
and colors as the CAN-health dot on a normal panel — green once
`mid_coach` is up and frames have been exchanged within the last 5 s, red
otherwise.

4. Bench-verify before trusting it on the coach: tap a button on
   `bedroom_remote` and confirm the corresponding real load actuates and
   the remote panel's own button visually confirms (round-trip status echo);
   then toggle the same load from `mid_coach`'s hardware button or the
   factory switch and confirm the remote panel's display updates.

The simulator can preview `bedroom_remote`'s layout (`cd sim; .\build.ps1
-Panel bedroom_remote -Run`) but cannot exercise the ESP-NOW link itself —
no radio in a PC build.

## If the board will not enter download mode

The ESP32-S3 usually auto-resets into the bootloader. If flashing fails to
start:

1. Hold **BOOT**, tap **RESET**, release **BOOT** — the board enumerates in
   download mode.
2. Flash again. After flashing, tap **RESET** to run the firmware.
3. If the port disappears mid-flash, lower the baud rate:
   `idf.py -p COM5 -b 115200 flash`

## Manual flash with esptool (recovery / no IDF environment)

Artifacts land in the build directory. From the repo root, after a build:

```powershell
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 build_mid_coach\bootloader\bootloader.bin 0x8000 build_mid_coach\partition_table\partition-table.bin 0x10000 build_mid_coach\firefly_touch.bin
```

Note the offset: the app lives at **`0x10000`**. Easier and less
error-prone — let the build directory supply them:

```powershell
cd build_mid_coach
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash "@flash_args"
```

Full chip erase (clears NVS and any stored state):

```powershell
idf.py -p COM5 erase-flash
```

## Firmware updates after installation

Panels are mounted **inside walls**, so an update means physically removing
one and reflashing over USB per the steps above. There is no OTA path: a
Wi-Fi STA + `esp_https_ota` transport was built and bench-tested (see
[CLAUDE.md](../CLAUDE.md) → *OTA / Wi-Fi update path*), but the target
network requires WPA3 + PMF, which the ESP32-S3's Wi-Fi radio (WPA2-only)
cannot join. Rather than weaken network security to work around that,
updates stay USB-only by design — `partitions.csv` is a single `factory`
app partition, and there is no Wi-Fi/OTA code in the firmware.

## Bench / sniffer builds

To verify the RV-C instance map or capture the factory PANEL LIGHTS frames,
enable sniffer mode — it logs **every** frame seen on the bus:

```powershell
idf.py -B build_mid_coach -DPANEL=mid_coach menuconfig
```

Navigate to *Firefly Touch Panel* → *RV-C sniffer mode*, enable, save, then
rebuild and flash. Output looks like:

```
I (10423) twai_tasks: RX id=0x19FEDA44 dgn=0x1FEDA sa=0x44 dlc=8 data=19 FF 64 FC FF 00 FC FF
```

`dgn=0x1FEDA` is `DC_DIMMER_STATUS_3`; the first data byte (`0x19` = 25) is the
RV-C instance. This is how the instance table in [CLAUDE.md](../CLAUDE.md) gets
confirmed against the real coach. Disable sniffer mode for production builds —
it is very chatty on a live bus.

## Adding a new panel

First decide which case you are in — this determines the source address, and
getting it wrong puts two nodes on the bus at the same address:

**A. Updating an existing panel** (new buttons, renamed labels, changed
instances). Edit that panel's existing header and **keep its current
`PANEL_INDEX`**. Nothing else below applies; `REGISTRY.md` needs no new row.

**B. Adding a genuinely new panel.** Continue:

1. Take the **next free index** from
   [`panels/REGISTRY.md`](../panels/REGISTRY.md).
2. Copy the template:

```powershell
Copy-Item panels\TEMPLATE.h panels\galley.h
```

3. Edit `panels/galley.h`: set `PANEL_NAME`, set `PANEL_INDEX` to the index
   from step 1, delete the `#error` line, and fill in `PANEL_BUTTONS[]` —
   label, type (`PANEL_BTN_DIMMER`, `PANEL_BTN_SWITCH`), and RV-C
   instances. The grid is 2 columns × 4 rows in reading order; backlight is
   automatic (see CLAUDE.md's UI section) so there's no manual
   lights/brightness button to wire up.
4. Add a row to `panels/REGISTRY.md` and bump its **Next free index** line.
5. Validate before building:

```powershell
python tools/check_panels.py
```

6. Preview the layout without hardware:

```powershell
cd sim; .\build.ps1 -Panel galley -Run
```

7. Build and flash:

```powershell
idf.py -B build_galley -DPANEL=galley -p COM5 flash monitor
```

8. Add the new loads to the instance table in [CLAUDE.md](../CLAUDE.md).

No C code changes are required. The build fails fast if `panels/<name>.h`
doesn't exist, and CI fails if the registry and headers disagree.

## Preview a panel layout before flashing

The PC simulator renders any panel header without hardware:

```powershell
cd sim
.\build.ps1 -Panel galley -Run
```

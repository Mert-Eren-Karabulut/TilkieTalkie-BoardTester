# Gauge chemistry: what bqStudio's database tells us (2026-10-13)

Question: can the LiFePO4 chemistry for the BQ28Z610 be built by the firmware from bqStudio's
chemistry database, without an EV2400 session? Looked at a bqStudio 1.3.1xx install
(`chemistry/` folder, the bq28z610 `.bqz`, the chemistry plugins).

## Findings
- `Chem.ini` is a plain-text index of 2491 chemistries: ID, description, the cells each ID was
  characterised on, two flags. 83 entries are LiFePO4 (`04xx`). `0418` = "LiFePO4/carbon",
  characterised on a PowerVar 32650 5000 mAh cell.
- `chemdat.txt` is a plain summary per chemistry: the flat open-circuit-voltage window used for
  Qmax updates, typical Qmax, Rhf. Not the tables.
- The tables themselves (`chemdat4/6/8/10/14/16`) are binary with a leading ID index and a
  payload at 8.00 bits/byte entropy: encrypted or compressed, no readable structure. The
  chemistry plugin jars only update the database; the programming is done by the native SDK
  (`bqSBB.dll`). The data is licensed for use within bqStudio only (`chemistry/Readme.txt`).
- Conclusion: the chemistry cannot and should not be reproduced by our firmware from the
  database. One bqStudio session with an adapter (EV2400 or MKST-3P-ALT-EV2300) is needed.

## What the session has to produce (once)
Either a data-flash flashstream (`*.df.fs`, Programming tab) after chemistry + learning cycle,
or simply one programmed gauge: the firmware can dump its whole data flash over I2C and clone
it into every other gauge at bring-up. The hidden chemistry regions are ordinary data flash:
from the bq28z610 map (`map.bcfgx`, base 0x4000): `OCRa` 0x4100 (Ra tables), `TempModel`
0x4400, `CellTable` 0x4480-0x4561 (OCV table), `IT` 0x4580 (Qmax etc.), configuration from
0x4600. So a full dump 0x4000-0x4800 carries chemistry, calibration and settings.

## Choosing the chemistry ID before the session
TI's `GPCCHEM` web tool picks the ID from a relax-discharge-relax log of the cell (voltage,
current, temperature every second, C/10 discharge). The firmware can record that log; no
adapter needed. Alternatively pick the entry whose characterised cell matches ours from the
local list (`docs/gauge/lfp-chem-ids.txt`, not committed).

## Firmware: the learned image is replayed at boot (2026-10-29)
`tools/gauge-image.py docs/gauge/dumps/<learned>.dfdump.txt --chem-id 0438` turns the dump into
`src/GaugeGoldenImage.{h,cpp}`: data flash 0x4100-0x47FF (Ra tables, State with Qmax / Update
Status, the chemistry tables of 0438, the IT and settings classes as validated on the calibration
unit) plus the expected values (chem ID, Qmax, Update Status, Ra flag) and the source's sha256.
0x4000-0x40FF (cell / pack / CC gains and offsets, lifetime counters) is never part of it: that
stays each unit's own.

`BatteryManager::replayGaugeGoldenImage()` runs in `applyPowerConfiguration()` after the
provisioning step, at every boot:
- chemistry 0438 + Update Status 06 + Ra flag 0x0055 → `applied`, nothing to do;
- chemistry 0438 + Update Status 05 → `learning` (a bench learning cycle in progress), untouched;
- anything else (stock 0x1210, or Update Status 04 after `gaugelearned confirm`) → `stock`: on
  USB the 56 blocks are written (32-byte MAC writes, each read back; ~5 s), DEVICE_RESET, and the
  chem ID / Update Status / Qmax / Ra flag are read back (`applied`, else `failed`). On battery
  the write waits for a boot on USB (the reset would drop the rail under the ESP).
- `gaugeimage inhibit` (bench console, NVS) blocks the replay on a unit that is about to run a
  learning cycle from Update Status 04; `gaugeimage allow` lifts it, `gaugeimage apply` forces a
  rewrite, `gaugeimage` alone prints the state. `ft begin` reports it as `gauge.image`.

Verified 2026-10-29 on the demo unit (stock 0x1210, Update Status 04): replayed at the first boot
on USB in 7 s, read-back OK, the gauge reported TrueFCC 4318 mAh right away, and a full dump
afterwards differs from the learned unit only inside 0x4000-0x40FF. The coulomb-counted SOC in the
firmware is the next thing to retire in favour of the gauge's.

## 2026-10-25: the learning cycles found a 300 mΩ pack, not a chemistry problem
Third learning discharge (chem 0438, `VDQ/QEN/VOK`, no `RDIS`, 430 mA via `learndis`): after
2.1 Ah out of a verified full the gauge collapsed to Remaining 0 / FCC 982 mAh / True Rem
-1145 with `TD, FD` set, the cell at 3.14 V under load. Cause, read from the gauge itself:
- Ra table learned this cycle (data flash 0x4100, flag 0x5555): grid 0-6 = 272-402 mΩ against
  57-90 mΩ in the baseline copy at 0x4140 (chem 0438 was characterised on a 14 mΩ cell,
  `chemdat.txt` Rhf). Two load steps confirm it independently: cycle-1 bqStudio log
  137→318 mA / 3292→3254 mV = 210 mΩ; ending this drain 431→183 mA / 3.144→3.198 V = 218 mΩ.
- With ~330 mΩ and `Load Select 7` (worst case: the highest average current seen, ~2 A from
  the reboot bursts of the cycle-1 brownout loop) the simulation reaches Term Voltage 2500 mV
  at 22 % depth of discharge, hence FCC = 0.22 × Qmax = 982 and "empty" at half.
- What the gauge's cell measurement spans: the cell, both leads and both connector contacts.
  The board's FET path sits between its BAT and PACK pins and measures ~37 mΩ (the earlier
  "100 mΩ charge path" figure used the PACK pin, which reads ~70 mV high at zero current).
- Verdict: the harness or the cell, decided by three meter readings at 1 A charge (cell
  terminals, board pads, cell terminals at rest). Then fix, re-apply the chemistry in bqStudio
  (resets Qmax/Ra/Update Status), one learning cycle (full, rest 5 h, discharge, rest 5 h).
- Product consequence: a pack like this makes the toy announce empty at half charge. The
  station should measure pack resistance with a load step (LEDs full for a few seconds,
  ΔV/ΔI from the gauge) and reject above a threshold set from a known-good harness.

Firmware side (bench build): `gaugedump [start end]` prints the data flash 0x4000-0x4800 as
bare `DF xxxx: ..` lines (64 MAC block reads; the golden-image copy through the board),
`gaugemac <cmd> [len]` prints one raw AltManufacturerAccess block, `gaugeit` decodes IT
Status 1-3 (true remaining/full capacity, Ra scale, DOD0/DODEOC, Qmax inputs).

### 2026-10-26 verdict: the cells
Second cell (the production battery, 20 AWG leads) on the same board: `ft test pack` 326 mΩ
(old cell 341). Meter directly on the cell terminals: 3.502 V while charging at 1 A, 3.255 V a
few minutes later while discharging at 0.4 A, and the board's battery pads read the same
3.255 V (gauge BAT pin 3.235-3.246, Cell1 3.204-3.216: the Cell1 register sits ~30 mV below
BAT at any current, a calibration offset). A 247 mV swing for a 1.45 A change, measured at the
cell itself, is ~170 mΩ steady-state; a sound 26700 LFP cell swings 30-50 mV. Harness and PCB
are cleared (no drop between cell terminals and pads, charger node within 30 mΩ of the cell).
Both cells of this type behave the same: the cell type/batch is high-impedance. Consequences:
the gauge's remaining-capacity simulation collapses under its worst-case load setting, and
1 A peaks (WiFi + audio) sag 0.2-0.3 V, which is the VSYSMIN regulation the charger logs near
empty. Next: source cells with DC internal resistance ≤ 20 mΩ, screen every pack with the
station's pack test, then redo chemistry + learning on a good cell.

### 2026-10-26 session 2: calibrated, reset, ready for the learning cycle
bqStudio (EV2400): voltage calibration against the meter (Cell 1, BAT and PACK, no current):
Cell1 now equals BAT (3500/3501 mV at 1 A charge, PACK +34 mV = the FET path), the old
-44/-12/+70 mV offsets are gone. Chemistry 0438 re-applied (Ra table back to the defaults, flag
FF55), Update Status 04, Qmax 4000, cycle count 0 set by hand (the chemistry update leaves
them), Load Select 7 → 1 (byte 0x4651; the IT class at 0x4580 is chemistry data, not these
settings). All verified through the board: `gaugedump` image kept in docs/gauge/dumps/
(2026-10-26-after-calibration-…). Charging at 1.05 A from ~72 % at 02:20 local; then 5 h rest
on the brick, flags checked (`VOK`, 0 mA), `learndis`, brick unplugged, LEDs to the trip, 5 h
rest, bqStudio: expect Update Status 06, export.

### 2026-10-27 02:35: cycle 4 result — Update Status 05, Qmax 4431, Ra table untouched
Discharge 4 (09:49-19:40, 0.43 A, `VDQ QEN VOK` all day; `RDIS` set at 3.11 V / ~95 % DOD
when fast scaling takes over, `VDQ` cleared ten minutes later, `TD`/`FD` after; slept at the
floor with the brick out) + 7 h rest: Update Status 04 → 05, Qmax 4431 (cycle 1 with the old
calibration: 4419, so the cells really hold ~4.4 Ah), rest reference taken (VOK, 96 % DOD).
The Ra table (0x4100) is still the chemistry default with flag FF55: this gauge updates Ra
only after Qmax has been learned (same as cycle 1 → 3). The VDQ drop at the end did not
block the Qmax update. One more cycle from 05 gives 06; then export.

### 2026-10-27: how VOK behaves on this gauge (saves a day next time)
`VOK` is not raised while the cell rests. It appears the moment the rest ends and current
flows, when the gauge commits the open-circuit reading: yesterday at the minute the top-off
began, this morning fifteen seconds after charging started at the bottom, and today fifteen
seconds after `learndis` started (`VDQ, QEN, VOK`). Waiting for `VOK` at 0 mA therefore never
ends (7.5 h lost today). The right readiness test after a charge: termination done (0 mA),
`REST` set, no `RDIS`, and the rested voltage drifting under ~1 mV per 5 min (3 h or more for
this LFP cell); then start the discharge and confirm `VDQ`/`VOK` in the first reading under
load. Cycle 5 discharge started 15:38 from Update Status 05 (Qmax 4431).

## 2026-10-29: cycle 6 → Update Status 06, the gauge is learned

Cycle 5 was lost on 2026-10-28 (a destructive console command sent by mistake two hours before
its end; Qmax 4418 / Update Status 05 written back by hand, docs/CONSOLE.md). Cycle 6, run by a
watcher script over the serial link: charge on the brick 01:27–05:56 (fast charge 1.06 A, taper,
gauge FC/TC at 3.60 V, charger stayed in CV hold with the LED on), rest 05:56–11:03 at 0 mA on
USB (RDIS cleared 06:01, VDQ + VOK raised 09:32, i.e. when the gauge finished its relax), `learndis`
11:03 (LEDs full white, −420 → −480 mA), brick unplugged 17:02 (must be out before the knee: a
brownout restart near the end re-enables the charger), 3.05 V at 19:45, TD at 20:39, ESP brownout
≈ 20:55 at ~2.8 V with 4193 mAh out; the firmware's cutoff then cut the cell (CUV mechanism,
1.3.13) and it rested cut off until 06:00.

Result after the boot on the brick (06:0x, cell 3.075 V, 312 mA charging):
- Update Status **0x06**, GaugingStatus 0x71815 (QMax updated, VDQ, QEN, VOK, TD, FD).
- Qmax 4443 mAh (cycle 4 gave 4431, written back as 4418), TrueFCC 4317 mAh, RaScale 1000.
- Ra table (0x4100, flag 0x0055 = learned, 15 grid points, 2^-10 Ω): 142 179 193 186 182 184 194
  201 214 225 216 222 282 339 597 → 139–219 mΩ over the plateau, 275 / 331 / 583 mΩ in the last
  three grids. Consistent with the 170 mΩ measured at the terminals (2026-10-26); the previous
  learned-then-collapsed table (272–402 mΩ with Load Select 7) is gone.
- The cutoff's CUV cut did not disturb the learning: the bottom rest with the DSG FET open was
  the second OCV point the gauge needed, Qmax and Ra were committed during it.

Images (all gitignored under docs/gauge/): `dumps/2026-10-29-learned-us06.dfdump.txt` (0x4000–
0x4800 via `gaugedump`, 64 blocks, 0 errors), `2026-10-29-learned-us06.gg.csv` (bqStudio Data
Memory export over the EV2400, 06:0x: Qmax 4443, Update Status 06, Load Select 1, Protection
Configuration 02, the Ra table above) and `2026-10-29-learned-us06.srec` (bqStudio golden image).
The `.bq.fs` format was not offered by this bqStudio build. Next: the firmware replay of this
image at bring-up so production units start learned (SHIP-CHECKLIST.md).

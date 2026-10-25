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

## Firmware plan (unchanged)
Flashstream/data-flash image replay at bring-up, versioned, verified by Chem ID, capacity,
Update Status, Qmax/Ra sanity; coulomb-counted SOC stays until then. See docs/FACTORY-MODE.md.

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

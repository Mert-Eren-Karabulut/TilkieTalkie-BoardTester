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

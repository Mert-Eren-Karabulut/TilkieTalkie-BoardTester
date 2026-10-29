# Hardware: Adem v7 board, pins, power parts, protection model

What the firmware assumes about the board, distilled from the 2026-10 audits and bench work.
Schematic sheets: [docs/schematic/esp32.png](schematic/esp32.png), [power.png](schematic/power.png),
[audio.png](schematic/audio.png), [nfc.png](schematic/nfc.png). Datasheets and TRM text dumps in
`datasheets/` (the BQ28Z610 TRM is `sluua65-bq28z610-trm.txt`; source comments cite it as "sluua65").

## Board
ESP32-S3-WROOM-1 N16R8 (16 MB QIO flash, 8 MB octal PSRAM), built with Arduino 3.3.x on ESP-IDF
5.5.5 through pioarduino (`platformio.ini`, partition table `default_16MB.csv`: two 6.25 MB OTA
apps, nvs, otadata, spiffs, coredump). The flash is an XMC-D part (ID 0x46): it supports
auto-suspend, which gave no gain on the ~300 ms LED freeze per 64 KB erase and is left off.

## Pin map (code matches the schematic)
| function | GPIO |
|---|---|
| battery I2C (bit-banged: BQ25792 charger, BQ28Z610 gauge, RT6160A, CH224Q) | SDA 1, SCL 2 |
| charger CE (active low, 2 MΩ pull-up: charging is off unless firmware drives it) | 44 (= U0RXD) |
| peripheral power enable: 4.5 V rail, SD, audio, NFC reset (one pin through 1N5819 diodes) | 4 |
| LED strip data (WS2812, driven straight over RMT) | 5 |
| pogo switch (figure on the pins) | 6 |
| buttons | 12, 13, 14, 21 |
| NFC I2C (PN532) | SDA 16, SCL 15 |
| codec I2C (ES8388) | SDA 17, SCL 18 |
| I2S | 7, 8, 10, 11 (C_ASDOUT 9) |
| SD (SD_MMC) | 48, 38, 42, 47, 39, 40 |
| AMP_CTRL (NS4150B), shared with the jack-detect contact | 41 |

## Power parts
- **BQ25792** charger: VREG set to 3.6 V by firmware (POR is 4.2 V, Li-ion; CE is asserted only
  after the read-back confirms 3.6 V), ITERM 120 mA, input limit per source (USB-PD through the
  CH224Q sink; `pdvolt 20` can request 20 V, check what hangs on VBUS first). Charging needs
  running firmware (CE on U0RXD); QON is not wired, so ship-mode exit is USB only, and Adem v7 has
  no ship FET on SDRV (only a capacitor): the charger's ship mode does nothing here.
- **BQ28Z610** Impedance Track gauge with an FS8205A FET pair between cell and PACK and a 2 mΩ
  low-side sense resistor. REGIN comes from the cell: a cell below ~2 V kills the gauge, and a
  gauge in SHUTDOWN only wakes when PACK sees a charger voltage (the firmware pulses the charger
  for that). The TS1 network is fixed resistors, not a thermistor (temperature reads a constant).
  FET Options must stay 0x00 (0x20 opened the CHG FET at full charge and sent the charger into a
  VBAT-OVP retry loop). A gauge DEVICE_RESET opens both FETs for a moment: on battery the board
  power-cycles.
- **2× RT6160A**: 3.3 V always on; the 4.5 V rail is enabled by GPIO4.
- **Cell** (`datasheets/batteryInfo.jpeg`): 4000 mAh LiFePO4, 3.65 V max, 0.2C / 0.5C / 1C
  charge, 2.0 V cutoff. The production cells read 320–340 mΩ on an 8 s load step and ~170 mΩ
  steady state at the terminals (docs/GAUGE-CHEMISTRY.md 2026-10-26): that is the cell, not the
  harness or the PCB. The gauge is configured for it (Load Select 1, learned Ra table).

## Protection model (firmware 1.3.13 and later)
- **Running:** low-battery cutoff at 3.00 V (debounced). The firmware raises the gauge's CUV
  threshold (0x46B3, 2500 → 3700 mV) so the gauge trips its own protection: DSG FET open, CHG FET
  closed, a charger always reaches the cell. If CUV has not tripped after 3 s a DEVICE_RESET makes
  the gauge re-read the threshold. `ft ship` cuts the cell the same way when USB is unplugged.
- **Boot on USB:** the threshold goes back to 2500 mV and the cell is reconnected (charger paused,
  CUV cleared on voltage, latch restored); a boot on battery leaves a cut alone. Monitor mode: the
  full system starts only when the cell can buffer WiFi, never while the cell is cut.
- **Off / asleep:** gauge CUV latch (Protection Configuration 0x46AE bit 1, re-applied every boot)
  and GPIO4 held low in deep sleep (4.5 V rail off).
- **No gauge answer on USB:** charging off, fault 115, the unit stays awake and reachable, with a
  300 ms charger pulse at boot and every 30 s to wake a gauge out of SHUTDOWN.
- **Deep-discharge recovery** is hands-free on USB; the gauge can take 15–30 s to come up.
- Never feed SYS from a second source while USB is connected (VSYS OVP); never deep-discharge a
  cell during handling.
- Charging voltage/current, taper and the LFP charge profile are written by the firmware at every
  boot (idempotent) on top of the learned gauge image (docs/GAUGE-CHEMISTRY.md).

## Observations for the next board spin (2026-10-11 audit, still open)
1. One GPIO (4) gates 4.5 V, SD, audio and NFC reset: nothing can be gated individually.
2. No thermistor on the gauge's TS1 and none on the charger's TS (fixed dividers).
3. Headphone jack on LOUT2/ROUT2 (never powered by firmware); the speaker gets the left channel.
4. AMP_CTRL (GPIO41) shared with the jack-detect contact.
5. MCLK through BAT46 + 10 kΩ + 33 Ω: marginal edges at 11.29 MHz.
6. SD pull-ups (10 k) to the always-on 3.3 V back-power the card in sleep.
7. Button RC (390 Ω + 100 nF) referenced to 3.3 V instead of GND.
8. No pull-down on REED_SW_MC on the main board (only on the top PCB).
9. Charger CE on U0RXD; QON not wired (ship-mode exit is USB only).
10. WS2812 data at 3.3 V into a strip on 4.5 V (V_IH 3.15 V): marginal.
11. Feed the gauge's REGIN from the charger side as well (diode-OR) so a dead cell cannot kill it.
12. Boot-mode strapping: a unit left in the ROM flasher stub after a USB flash needs a proper
    reset (the station does DTR/RTS; esptool `--after hard_reset`).
13. First NFC board batch: the PN532 module's regulator output was tied to the board supply
    (back-feed drained the cell in sleep); corrected on later boards.

# Manufacturing: the test station, step by step

The technician's procedure for every unit between "assembled" and "in the box". The station is the manufacturing panel of the portal (`/admin/manufacturing`, Chrome with
WebSerial); it speaks the `ft` protocol of docs/FT-PROTOCOL.md over the USB cable. You never type
console commands on a production unit: the panel does it, and the bench commands (`bringupreset`,
`gaugeprog`, `startsystem`, ...) do not exist in production firmware.

## 1. What the station needs

| item | why |
|---|---|
| PC with Chrome, logged in to the portal as an admin | the panel (WebSerial, ESP Web Tools for flashing) |
| USB-C cable + a 5 V / 2 A or USB-PD supply path (a powered hub or the PC port if it delivers) | power, charge current for bring-up, the serial link. One cable does all three |
| the test card (an NFC tag in a figure base) | `nfc` and `pogo` |
| a 3.5 mm plug (any headphone) | `jack` (may be skipped when the station has none) |
| a 2.4 GHz test network: SSID without spaces, WPA2 or open, with internet that reaches the portal | `wifi` (the probe fetches `http://<portal>/storage/ping.json`) |
| a phone with a BLE scanner (optional) | to see the advertisement during `ble` (may be skipped) |
| label printer | step 6 |
| SD cards prepared with `tools/sdcard-prepare.sh` (the current prompt pack) | `sd` checks the pack on the card |

The unit: cell fitted and connected, **not full and not empty** (storage charge, 30-70 %), SD card
inserted, figure base (pogo pins) and buttons assembled. Room temperature.

Never feed an external voltage into SYS while USB is connected: USB-C only (double feeding trips
the charger's VSYS OVP).

## 2. LED legend during the station

| LED | meaning |
|---|---|
| five LEDs dim white, steady | factory session open, nothing running (the panel is connected) |
| solid blue | bring-up: provisioning the gauge and loading the learned gauge image (a few seconds) |
| amber, breathing | bring-up: waiting for charge current (USB power, cell not full) |
| solid cyan | bring-up: calibrating the current sense (a few seconds) |
| solid green (briefly) | bring-up passed |
| red flashes (5x) | bring-up gave up (the panel shows the reason) |
| the walk red / green / blue / white, then all white | the `leds` test |
| green, breathing | ship mode armed: unplug USB now |
| blue breathing (no session) | battery-monitor mode: the unit waits for bring-up or for a healthy cell |

## 3. The procedure (what the panel does, what you do)

### Step 1: connect, optionally flash

1. Plug the unit in, press **Connect** in the panel and pick the serial port.
2. **Flash** (new boards, or a unit with a wrong firmware): the panel writes the release's USB
   image with ESP Web Tools (merged image at offset 0, erase first). The erase also removes any
   bench leftovers (endpoint override, gauge bus release, volume/brightness). After flashing the
   panel reconnects by itself; if it does not, press Connect again.

Do not flash an enrolled unit: the erase deletes its factory identity and records (NVS), so it
would need a new enrollment and a new label. A unit that already runs the release skips this step.

### Step 2: open the session, bring-up

The panel sends `ft begin`. The unit answers its state and switches to factory mode: dim white
LEDs, no sleep, no voice, no updates, figures and buttons do nothing on their own. Check that the
panel shows the expected firmware version.

If the bring-up state is not **done**, the panel runs the `bringup` test (up to 3 minutes):

| progress | what happens | you |
|---|---|---|
| provision | the gauge is configured for the 1S LiFePO4 cell | wait |
| wait_charge | the unit needs a steady charge current to calibrate | check the USB power; the cell must not be full |
| calibrate | the current sense is calibrated against the charger | wait |
| complete | stored; charge current raised to 1 A | — |

A fresh board usually takes well under a minute; a board that was brought up before finishes in
seconds. The result (state, times, gauge data, failure reason) is stored on the unit in the factory
record `bringup` and shown by the panel.

Bring-up failures:

| reason | meaning | do |
|---|---|---|
| `no USB power, no charge current in 3 min` | the charger sees no VBUS | other cable / supply, re-run |
| `cell full, no charge current in 3 min` | a full cell takes no current, calibration needs ~300 mA | discharge the cell a little (or use a cell at storage charge), re-run |
| `no charge current in 3 min` | VBUS present but the charger does not charge | check the cell connector; run `battery` to see the charger faults; rework if it repeats |
| `gauge provisioning does not stick` | the gauge answers but rejects its configuration | rework (gauge / cell connector) |
| `gauge not answering (no USB, 3 min)` | the gauge never left ROM mode on battery | plug USB in, re-run; if it stays, rework |
| `current calibration failed 5 times` | the charge current was not steady enough | supply that sags, or a cell at the end of charge; re-run with a lower charged cell |
| `budget 3 min exceeded in <phase>` | the test's own limit | look at the phase: same remedies as above |

A unit whose bring-up failed still works (it falls back to normal operation with fault 508
BRINGUP_FAILED and charges at 300 mA only); it must not be shipped until a bring-up passes.

The gauge chemistry is **not** checked here: the firmware does not program a chemistry, the gauge
reports whatever ID it shipped with (the bring-up record shows it as `chem_id`) until the golden
data-flash image is replayed by the firmware itself (docs/GAUGE-CHEMISTRY.md).

### Step 3: enroll

The panel registers the unit on the portal (`POST /api/factory/hubs`, the portal assigns the
serial `TT<yy>-<5 digits>`) and writes the identity with `ft enroll <secret> <pop> <serial>`.
Check that the panel shows the serial and the pairing code `TK-XXXX-XXXX`.

Re-enrolling a unit (it was enrolled before) gives it a new secret and a new pairing code: its
old label is dead and must be replaced (step 6).

### Step 4: the self-tests

The panel runs them in this order. Every test can be repeated; a re-run replaces the earlier
result. `jack` and `ble` may be skipped when the station has no plug or phone.

| test | you | passes when | on failure |
|---|---|---|---|
| `battery` | — | gauge answers, cell 3000-4250 mV, -5..50 °C, no charger fault | `gauge not answering`: cell connector / gauge. Voltage or temperature out of range: check the cell. `charger fault: ...`: VBUS/VBAT OVP = supply or cell problem, others = rework |
| `pd` | — | VBUS ≥ 4.5 V (mode PD/QC/BC/none is recorded) | below 4.5 V: cable or supply; `none` with 5 V is fine |
| `sd` | — | 2 MB write ≥ 0.2 MB/s, read ≥ 1 MB/s, read-back identical, prompt pack complete | `no card`: card missing or unreadable at boot (reseat, restart, re-run). Slow write/read: replace the card. `no prompt pack` / `incomplete`: re-prepare the card with `tools/sdcard-prepare.sh` |
| `leds` | watch: each LED red, green, blue, white in turn, then all white; answer **OK** / **Fail** | you say OK | a dead or wrong-colour LED: rework (LED chain or solder) |
| `buttons` | press each of the 4 buttons once (any order, 30 s per button) | all 4 seen | the missing numbers are shown: button or its contact |
| `pogo` | put the test card's figure on the pins, then lift it | closed and opened seen within 20 s | the pogo switch or its spring |
| `nfc` | put the test card on the pins | the reader answers (version shown) and the card's UID is read within 20 s | `reader not answering`: PN532 / its I2C. No card read: card position, antenna |
| `speaker` | listen: a 1 kHz tone, then the greeting; answer OK / Fail | you say OK | no sound: speaker, amp, codec. Distorted: speaker. No greeting (`prompt:false`): the card has no pack (see `sd`) |
| `jack` | insert the plug, then pull it out | both seen within 20 s | the jack contact (it switches the speaker amp) |
| `wifi` | — (the panel sends the test network) | connected within 20 s and the portal probe returned our answer | `no connection`: SSID/password, distance, antenna. `captive or foreign answer`: the test network intercepts HTTP. `no answer`: the network has no internet or the portal is down |
| `ble` | optional: see the unit's name in the phone's BLE scanner | the BLE stack started and advertised 10 s | `BLE released on this boot`: the unit was paired before; it needs `ft clean`, then a new session |
| `sleep` | — | always fails (`not implemented`: no current shunt on this hardware) | not run by the panel |

The `wifi` test never changes the network the unit has stored (a customer's, on a returned unit):
it runs on a temporary configuration and puts the previous one back.

An `ask` (leds, speaker) waits 120 s for your answer, then fails with `no answer in 120 s`.
**Abort** stops the running test (`aborted`).

### Step 5: commit

`ft commit` stores the test record on the unit (factory record `tested`: time, firmware, pass,
every test), and the panel sends the results with the operator and the station to the portal
(`POST /api/factory/hubs/{uuid}/test`). The next `ft begin` of this unit shows the record.

### Step 6: label

Print the label: QR `TT1:<uuid>:TK-XXXX-XXXX`, below it the serial, the pairing code and the uuid.
Stick it on the unit. A unit without a passed record does not get a label.

### Step 7: clean, ship mode, unplug

- `ft clean` is not needed on a fresh unit. It is for a unit that was used (returned, or paired
  during rework): it forgets WiFi, the device token, the card cache, the fault ring and the volume
  settings, keeps the factory identity, and restarts (the panel reconnects and opens a new session).
- `ft ship` (cell at least 3.4 V, otherwise `charge first`: leave it charging and retry): the LEDs
  breathe green, **unplug the USB cable within 10 seconds**. The unit switches off within a few
  seconds (ship mode, µA drain; it may light up once more for a second before it goes dark, that
  is normal). It wakes when USB is plugged in again.
- `still powered`: the cable was still in after 15 s; ship mode was cancelled. Run `ft ship` again
  and unplug right after the answer.

## 4. Re-test and rework

- A unit coming back to the station: Connect, `ft begin` shows its identity, bring-up state and
  the last test record. Run the tests that matter and commit again (the record is replaced).
- Re-enrolling rotates the secret and the pairing code: reprint the label.
- `ft reset --all` (service only) also erases the factory identity and records; the unit then
  needs a full enrollment again.
- A reboot ends the session: after a restart the panel sends `ft begin` again.

## 5. Where the records are

| record | where | content |
|---|---|---|
| identity | NVS namespace `factory`: `secret`, `pop`, `serial`, `enrolled` | written by `ft enroll`, survives the customer reset and `ft clean`, erased only by `ft reset --all` or a flash erase |
| `bringup` | same namespace | the last bring-up run: state, reason, trigger (boot / ft / console), start and end time (unix, 0 without a clock), duration, firmware, gauge (present, provisioned, calibrated, chem_id, design capacity, mV, °C, current), charger (VBUS, charge current, state), attempts |
| `tested` | same namespace | the last committed test record, at most 1.5 KB (the per-test data is left out when it would not fit) |
| portal | the hub's page | enrollment, the uploaded test results, operator, station |

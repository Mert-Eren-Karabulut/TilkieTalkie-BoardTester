# `ft`: the factory test protocol (USB serial, 115200)

The manufacturing panel on the portal (`/admin/manufacturing`, WebSerial in Chrome) drives a
unit through flashing, bring-up, identity, self-tests, label and ship mode over the console.
Both repos implement this file; PROTOCOL-V2.md §9 points here.

## Transport and framing

- The station sends one command per line: `ft <verb> [args]\n`. Arguments never contain spaces
  except the last one when noted.
- The device answers every command with **one or more JSON lines that start with `{"ft":`**.
  Everything else on the port (ESP_LOG lines, boot noise) is ignored by the station. While a
  session is open the device lowers its log level to WARN so the port stays quiet.
- Shapes: `{"ft":"<verb>","ok":true,...}` on success, `{"ft":"<verb>","ok":false,"error":"<text>"}`
  on refusal. Unknown verb: `{"ft":"?","ok":false,"error":"unknown ft command"}`.
- Timeouts: the station waits 5 s for the first line of any command, except tests, which have
  their own budgets below.

## Session

- `ft begin` → opens a factory session and answers the unit's state:
  ```
  {"ft":"begin","ok":true,"uuid":"250696234569532","mac":"3C:0F:02:C4:01:E4","hw":"adem-v7",
   "fw":"1.3.5","code":10305,"enrolled":false,"serial":null,"pop":false,
   "bringup":{"state":"done|pending|failed|running","record":{...}|null},
   "card":{"present":true,"size_mb":29820,"free_mb":29500},
   "pack":{"checksum":"c1f1888c0884...","files":28,"missing":0},
   "battery":{"mv":3912,"soc":48,"temp_c":26,"charging":true,"gauge":true},
   "vbus_mv":5080,"session":true}
  ```
  Factory mode while the session is open: no deep sleep, no OTA checks or jobs, no voice
  prompts or hello, no figure handling (NFC events still feed the `nfc`/`pogo` tests), LEDs show
  a dim white "factory" idle scene, WiFi and BLE untouched until a test asks for them.
  `ft begin` twice is fine (same answer). A reboot ends the session.
- `ft end` → `{"ft":"end","ok":true}`; normal operation resumes (log level back to INFO).
- `ft show` (no session needed) → identity: `{"ft":"show","ok":true,"uuid","mac","serial","enrolled","label"}` (never the secret; `label` is `TT1:<uuid>:TK-XXXX-XXXX` or "" when no pop).
- `ft enroll <secret> <pop> [serial]` → stores the factory identity (pop accepted as `TK-XXXX-XXXX`
  or 8 bare characters, stored bare) → `{"ft":"enroll","ok":true,"uuid","serial","label"}`.
- `ft reset [--all]` → customer reset (keeps the factory namespace); `--all` wipes it too. Restarts.

## Tests

- `ft test <name> [args]` starts one test; only one runs at a time (`{"ok":false,"error":"busy"}`
  otherwise). The device answers immediately `{"ft":"test","name":"<name>","state":"running"}`,
  then zero or more `{"ft":"progress","name":"<name>","step":"<text>","done":n,"total":N}`, and
  exactly one final `{"ft":"result","name":"<name>","pass":true|false,"ms":<duration>,
  "data":{...},"error":"<text when pass is false>"}`.
- A test that needs the operator's eyes or ears ends its automatic part with
  `{"ft":"ask","name":"<name>","question":"<text>"}` and waits up to 120 s for `ft ok` or
  `ft fail [reason]`, after which the `result` line follows (pass = ok).
- `ft abort` cancels the running test → its result line with `"error":"aborted"`.
- Results are kept in RAM per name (a re-run overwrites) until `ft commit` or `ft end`.

| name | what the device does | pass rule | data |
|---|---|---|---|
| `leds` | walks the 5 LEDs: red, green, blue, white one by one (150 ms each), then all white; asks "Did all 5 LEDs light red, green, blue and white?" | operator ok | `{"count":5}` |
| `buttons` | waits up to 30 s for each of the 4 buttons once, any order; progress `"press button n"` after each press | all 4 seen | `{"seen":[1,2,3,4],"ms":[...]}` |
| `nfc` | PN532 self-test (firmware version), then waits up to 20 s for a card on the pins | reader answers and a uid is read | `{"reader":"1.6","uid":"04-..."}` |
| `pogo` | waits up to 20 s for the pogo switch to close and then open again | both seen | `{"closed_ms":<ms held>}` |
| `speaker` | 1 kHz tone 1 s at a safe volume, then the `idle_hello` prompt when the pack has it; asks "Was the tone and the greeting audible and clean?" | operator ok | `{"prompt":true}` |
| `jack` | waits up to 20 s for a plug to be inserted and removed (AMP_CTRL level) | both seen | `{"inserted_ms":<ms>}` |
| `battery` | reads the gauge (voltage, soc, temperature, design capacity, status) and the charger (status, faults, VBUS, charge current) | gauge answers, 3000 ≤ mv ≤ 4250, -5 ≤ temp ≤ 50, no latched charger fault | `{"mv","soc","temp_c","design_mah","charger":{"status","faults","ichg_ma"},"vbus_mv"}` |
| `pd` | CH224Q handshake and VBUS | VBUS ≥ 4500 mV | `{"mode":"PD|QC|BC|none","vbus_mv"}` |
| `sd` | card present; writes and reads back 2 MB; pack.json present and complete | write ≥ 0.2 MB/s, read ≥ 1 MB/s, pack complete | `{"size_mb","write_mbps","read_mbps","pack":"<checksum>","missing":0}` |
| `wifi <ssid> <pass>` | connects to the given network (20 s), probes `http://<endpoint host>/storage/ping.json` as PROTOCOL-V2 §6, disconnects; the stored customer credentials are not touched; `<pass>` may be empty for an open network; `<ssid>` may not contain spaces | connected and probe body matched | `{"rssi","ip","probe":true,"ms"}` |
| `ble` | starts the provisioning advertiser for 10 s under the normal device name, then stops it | the stack started | `{"name":"..."}` |
| `bringup` | runs the bring-up state machine again (gauge provisioning, charge current, calibration) with a 3 min budget; progress per state; the record is written to the factory namespace | reached the done state | `{"state","record":{...}}` |
| `sleep` | not implemented on this hardware (no shunt) | always fails with `"error":"not implemented"` | |

## Results, commit, end of line

- `ft result` → `{"ft":"results","ok":true,"pass":<all passed>,"tests":{"leds":{"pass":true,"ms":...,"data":{...}},...}}`.
- `ft commit` → writes `{"tested_at":<unix or 0>,"fw":"1.3.5","pass":bool,"tests":{...}}` into the factory namespace (key `tested`) → `{"ft":"commit","ok":true}`. `ft begin` reports it under `"tested"`.
- `ft clean` → same as `ft reset` (factory namespace kept): WiFi, token, card cache, fault ring, volume gone; restarts → `{"ft":"clean","ok":true}` first.
- `ft ship` → arms BQ25792 ship mode **with the 10 s delay** (SDRV_DLY = 0) and answers
  `{"ft":"ship","ok":true,"unplug_within_s":10}`; the operator unplugs USB within 10 s and the unit
  powers down to ship mode (wake = plugging USB in). If VBUS is still present after 15 s the
  device cancels and sends `{"ft":"ship","ok":false,"error":"still powered"}`. Refused while a
  test runs or when the cell is under 30 % state of charge or 3.2 V (`"error":"charge first"`;
  LiFePO4 rests near 3.3 V from half charge up, so the gauge's state of charge is the gate).

## Label

The QR payload is `TT1:<uuid>:TK-XXXX-XXXX`; the human-readable lines are the serial, the pairing
code `TK-XXXX-XXXX` and the uuid. Serials are assigned by the portal at enrollment:
`TT<yy>-<5 digits>` (e.g. `TT26-00042`), unique, sequential.

## Station flow (the panel)

1. Connect (WebSerial) → optional flash with ESP Web Tools using the release's USB image (merged
   image at offset 0, erase first) → reconnect.
2. `ft begin`; if `bringup.state` is not done: `ft test bringup`.
3. Enroll: `POST /api/factory/hubs` (portal assigns the serial) → `ft enroll <secret> <pop> <serial>`.
4. Tests in this order: battery, pd, sd, leds, buttons, pogo, nfc, speaker, jack, wifi, ble.
   Any failure can be retried; the operator may skip `jack`/`ble` when the station has no plug/phone.
5. `ft commit` → `POST /api/factory/hubs/{uuid}/test` with the results, the operator and station.
6. Print the label.
7. `ft clean` is not needed on a fresh unit; `ft ship`, unplug.

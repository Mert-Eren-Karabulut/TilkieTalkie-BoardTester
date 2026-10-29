# Serial console

USB CDC, 115200 baud (`pio device monitor`). One command per line; only the command word is
lowercased (URLs, paths and hex arguments keep their case). `help` prints the list compiled
into the running image. Code: `src/Console.cpp`, entry point `Console::service()` from `loop()`.

## Two builds

| env | flags | use |
|---|---|---|
| `esp32-s3-devkitc-1` | (none) | production: releases (`tools/release.sh`), OTA, USB flash images |
| `bench` | `-DTT_BENCH` | bench boards: production plus the bench commands below |

`pio run -e bench -t upload` flashes a bench image. Both envs share
`sdkconfig.esp32-s3-devkitc-1` (`board_build.esp-idf.sdkconfig_path`), so the images differ
only in the console (and the bench-only `sleeptest` report at boot, `chargemon` and the
learning-discharge floor in the loop).

Persisted bench state stays removable from a production image: `endpoint clear` drops an
`endpoint set` override and `gaugebus attach` ends a `gaugebus release`.

Confirmations (`factory`, `delete`, `deletefig`, `poweroff`; bench) wait at most 10 s for `yes`,
feeding the loop watchdog meanwhile; any other answer or silence cancels.

## Production commands

| group | commands |
|---|---|
| system | `help`, `version`, `faults`, `restart`, `config` (no PIN), `debug`, `efusemac` / `hubid`, `heap`, `stack`, `timing`, `tasks`, `diag [on\|off]`, `coredump`, `coredumperase`, `endpoint`, `endpoint clear`, `ota`, `ota check [release_id]` |
| factory (`ft`) | `ft begin`, `ft end`, `ft show`, `ft enroll <secret> <pop> [serial]`, `ft test <name> [args]`, `ft ok`, `ft fail [reason]`, `ft abort`, `ft result`, `ft commit`, `ft clean`, `ft ship`, `ft reset [--all]` (below) |
| WiFi / Reverb | `qr`, `reset`, `stats`, `reverbstatus`, `reverbclean`, `reverbstart` |
| battery / power | `battery`, `battscan`, `batterycfg`, `batteryclearflags`, `chargekick`, `gaugefets`, `gaugeprot`, `gaugecuvfix`, `gaugecutoff`, `gaugedfbyte`, `gaugebus`, `gaugebus attach`, `pdstatus`, `power`, `poweron` |
| sleep | `sleep`, `sleep now`, `sleepafter <ms>`, `cancelsleep`, `sleepstatus` |
| files | `sdtree`, `sdinfo`, `sdbench`, `sdwritebench [chunkKB] [count]`, `dlstats`, `dlqueue`, `dlclear`, `required`, `checkfiles`, `cleanup` |
| audio | `play <path>`, `play`, `pause`, `resume`, `stop`, `next`, `prev`, `playlist`, `track`, `volup`, `voldown`, `volume`, `volume <n>`, `audio`, `amp on\|off\|auto` |
| LED | `ledon <hex> <0-255>`, `ledoff`, `pulse <hex>`, `rapid <hex> <count>` |
| NFC | `nfcstatus`, `nfcdata`, `nfcreed` / `nfcpogo`, `nfcdiag`, `nfcsleep` |

`faults` prints the active faults and the recent fault events (docs/FAULTS.md).

`hubid` prints the hub uuid (the efuse MAC in decimal), the MAC in the usual byte order
(1.2.19 and older printed it reversed), whether the unit is enrolled, and the label payload
`TT1:<uuid>:TK-XXXX-XXXX` once a pop is enrolled.

### Factory protocol `ft` (docs/FT-PROTOCOL.md)

The contract with the portal's manufacturing panel is docs/FT-PROTOCOL.md (shapes, the test
catalogue, pass rules); the station procedure is docs/MANUFACTURING.md. Every answer is one or
more JSON lines starting with `{"ft":"<verb>"` (`{"ft":"<verb>","ok":false,"error":"..."}` on a
refusal, `{"ft":"?","ok":false,"error":"unknown ft command"}` for an unknown verb), written
straight to the port so the log level never hides them. Implemented in src/FactoryTest.cpp.

| command | session needed | answer |
|---|---|---|
| `ft begin` | no (opens it) | the unit's state: uuid, mac, hw, fw, code, enrolled, serial, pop, bringup {state, record}, card, pack, battery, vbus_mv, tested (when committed), session. Again while open: the same answer |
| `ft end` | no | `{"ft":"end","ok":true}`; a running test ends as aborted; normal operation resumes |
| `ft show` | no | `{"ft":"show","ok":true,"uuid","mac","serial","enrolled","label"}`; serial null and label "" until enrolled; never the secret |
| `ft enroll <secret> <pop> [serial]` | no | stores the factory identity (NVS namespace `factory`) → `{"ft":"enroll","ok":true,"uuid","serial","label"}`. secret: 16-128 characters of `A-Za-z0-9-_.` (the portal's base64url secret); pop: `TK-XXXX-XXXX` or `XXXXXXXX` from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, stored as the bare 8 upper-case characters (the BLE proof of possession); serial: optional, up to 32 of the same characters |
| `ft test <name> [args]` | yes | `{"ft":"test","name","state":"running"}`, progress lines, maybe an `ask`, one `result` line. Names: leds, buttons, nfc, pogo, speaker, jack, battery, pd, sd, `wifi <ssid> <pass>`, ble, bringup, sleep. Refused with `busy` while a test (or ship) runs, `system not started` for the tests that need the full system while the unit is still in battery-monitor mode |
| `ft ok` / `ft fail [reason]` | yes | answers an `ask`: `{"ft":"ok","ok":true}` then the result line |
| `ft abort` | — | `{"ft":"abort","ok":true}` then the running test's result with `"error":"aborted"` |
| `ft result` | yes | `{"ft":"results","ok":true,"pass":<all passed>,"tests":{...}}` (pass is false with no results) |
| `ft commit` | yes | the record `{"tested_at","fw","pass","tests"}` into the factory key `tested` (≤ 1.5 KB; the per-test data is dropped if it would not fit) → `{"ft":"commit","ok":true}`; the RAM results are cleared |
| `ft clean` | no | `{"ft":"clean","ok":true}`, then the customer reset below and a restart |
| `ft ship` | yes | `{"ft":"ship","ok":true,"unplug_within_s":10}` and BQ25792 ship mode with the 10 s delay; `still powered` (cancelled) when VBUS is still there after 15 s; refused with `charge first` under 3.4 V and `busy` during a test |
| `ft reset` / `ft reset --all` | no | `{"ft":"reset","ok":true,"reset":"customer"\|"all"}`, then the customer reset (with `--all` also the factory namespace: secret, pop, serial, test and bring-up records) and a restart |

Factory mode while a session is open: no deep sleep, no OTA checks or jobs, no voice prompts
or hello, no figure handling (the buttons, the NFC reader and the pogo switch only feed the
tests), a dim white LED scene, WiFi and BLE left as they are (a unit whose full system starts
during the session starts WiFi only at `ft end`), log level WARN. A reboot ends the session.

## Button holds (volume buttons 1 + 4)

| hold | what |
|---|---|
| 5 s | a tone (two rising notes): "forget WiFi" is armed; letting go before 10 s forgets the network (stories stay), says `wifi_reset_done` and restarts into BLE provisioning |
| 10 s | a second tone (three falling notes): **customer reset**: best-effort `POST /api/hubs/{uuid}/unbind` with the device token (3 s, skipped offline), device token and its age deleted, card cache wiped (stories, manifests, download queue, uid mappings; not the prompt pack, not `/.device`), fault ring cleared, volume / volume ceiling / brightness settings deleted, WiFi forgotten, restart into provisioning. The factory namespace stays. The app/admin command `factory-reset` and `ft reset` run the same reset |

Buttons 2 + 3 held 5 s restart the toy (unchanged).

## Bench commands (`-DTT_BENCH` only)

| command | what |
|---|---|
| `endpoint set <api_base> <reverb_host> <port> <tls 0\|1> [key]` | point the unit at a bench portal (NVS; `restart` to apply); the portal probe follows it (`http://<host>:<port>/storage/ping.json` for an http base): until it answers `{"ping":"tilkie"}` the toy is offline; an answer that is not ours = captive portal (rejected while pairing), no answer = no internet (accepted while pairing, re-probed every 2 min) |
| `sdformat` | delete everything on the card (no confirmation) |
| `download <url> <path>`, `addfile <path> <url>`, `deletefile <path>` | manual file management (any URL, any path) |
| `ota url <url> <sha256> <size> [version]` | install an arbitrary image |
| `testauth` | sends the token to `/user` without certificate checks |
| `send <message>` | chat broadcast (the portal refuses chat from a device: 401/403) |
| `config` | also prints the legacy provisioning PIN |
| `factory` | erase all stored data (asks `yes`) |
| `delete` | delete all required files from NVS and the card (asks `yes`) |
| `deletefig <uid>` | delete one figure's cached stories + manifest (asks `yes`) |
| `poweroff` | switch the peripheral rail off (asks `yes`) |
| `cat <path>` | print the first 4 KB of a file on the card |
| `tap <uid>`, `untap` | simulate mounting / removing a figure |
| `startsystem` | start the full system bypassing the battery gate |
| `bringupreset` | clear the bring-up flag and re-run the gauge bring-up (production: `ft test bringup`) |
| `gaugebus release` | hand the battery bus to bqStudio / EV2400 (persists across reboots) |
| `gauge1s`, `gaugeprog`, `gaugeresetlearn confirm`, `gaugelearned confirm`, `gaugecalcurrent` | gauge provisioning tools. The two `confirm` ones seed Qmax back to Design Capacity, write Update Status 0x04 and reset the gauge: the learned state and any learning cycle in progress are gone. Without `confirm` they only print a warning; `gaugeit` and `battery` are the read-outs |
| `dfread <hexaddr> [len]`, `dfwrite8 <hexaddr> <hexval>` | gauge data flash |
| `gaugecutoff` | the cutoff / ship-mode cut while on USB (CUV threshold raised to 3700 mV: gauge DSG FET open, CHG FET closed; the full system then usually power-cycles on the USB-only rail and the boot reconnects the cell, which is the field path); `gaugefets` restores by hand |
| `gaugedfbyte <hexaddr> [hexval]` | read, or write and read back, one gauge data-flash byte (e.g. `gaugedfbyte 46AE` Protection Configuration) |
| `gaugeimage [apply\|inhibit\|allow]` | the learned gauge image (src/GaugeGoldenImage.h, docs/GAUGE-CHEMISTRY.md): state (`applied`, `learning`, `stock`, `inhibited`, `failed`), forced rewrite, or the NVS inhibit for a unit about to run a learning cycle from Update Status 04 (the boot would otherwise replay the image) |
| `gaugedump [hexstart hexend]` | hex dump of the gauge data flash as bare `DF 4100: ..` lines, default `4000 4800` (the whole image: calibration, OCV/Ra tables, learned state, configuration); capture the log to keep a golden image copy |
| `gaugemac <hexcmd> [len]`, `gaugeit` | one raw AltManufacturerAccess block / Impedance Track internals decoded from IT Status 1-3 (true remaining and full capacity, Ra scale, depth-of-discharge reference points, Qmax inputs) |
| `learndis`, `learnrest`, `learnchg` | gauge learning cycle (discharge stops at 2.60 V, charger in HIZ so USB can stay connected) |
| `sleeptest <min> [railon] [nfcawake] [hold]` | sleep current via the gauge; result prints on the wake boot |
| `chargemon [seconds]` | one charger/gauge line per second (max 300 s) |
| `pdvolt <5\|9\|12\|15\|20>` | request another USB-PD voltage |
| `crash` | `abort()` on purpose (rollback / crash-loop guard tests) |

In a production image these fall through to "Unknown command" (`endpoint set` says it is a
bench command; `gaugebus release` prints the bus status).

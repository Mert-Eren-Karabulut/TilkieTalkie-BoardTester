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
| factory (`ft`) | `ft enroll <secret> <pop> [serial]`, `ft show`, `ft reset [--all]` (below) |
| WiFi / Reverb | `qr`, `reset`, `stats`, `reverbstatus`, `reverbclean`, `reverbstart` |
| battery / power | `battery`, `battscan`, `batterycfg`, `batteryclearflags`, `chargekick`, `gaugefets`, `gaugeprot`, `gaugecuvfix`, `gaugebus`, `gaugebus attach`, `pdstatus`, `power`, `poweron` |
| sleep | `sleep`, `sleep now`, `sleepafter <ms>`, `cancelsleep`, `sleepstatus` |
| files | `sdtree`, `sdinfo`, `sdbench`, `sdwritebench [chunkKB] [count]`, `dlstats`, `dlqueue`, `dlclear`, `required`, `checkfiles`, `cleanup` |
| audio | `play <path>`, `play`, `pause`, `resume`, `stop`, `next`, `prev`, `playlist`, `track`, `volup`, `voldown`, `volume`, `volume <n>`, `audio`, `amp on\|off\|auto` |
| LED | `ledon <hex> <0-255>`, `ledoff`, `pulse <hex>`, `rapid <hex> <count>` |
| NFC | `nfcstatus`, `nfcdata`, `nfcreed` / `nfcpogo`, `nfcdiag`, `nfcsleep` |

`faults` prints the active faults and the recent fault events (docs/FAULTS.md).

`hubid` prints the hub uuid (the efuse MAC in decimal), the MAC in the usual byte order
(1.2.19 and older printed it reversed), whether the unit is enrolled, and the label payload
`TT1:<uuid>:TK-XXXX-XXXX` once a pop is enrolled.

### Factory protocol `ft` (docs/PROTOCOL-V2.md §9, first commands)

One command per line, one JSON line back on the console: `{"ok":true,...}` or
`{"ok":false,"error":"..."}`.

| command | answer |
|---|---|
| `ft enroll <secret> <pop> [serial]` | stores the factory identity (NVS namespace `factory`) and answers like `ft show`. secret: 16-128 characters of `A-Za-z0-9-_.` (the portal's base64url secret); pop: `TK-XXXX-XXXX` or `XXXXXXXX` from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, stored as the bare 8 upper-case characters (the BLE proof of possession); serial: optional, up to 32 of the same characters |
| `ft show` | `{"ok":true,"uuid":"...","mac":"AA:BB:CC:DD:EE:FF","serial":"..."\|null,"enrolled":true\|false,"label":"TT1:<uuid>:TK-XXXX-XXXX"\|null}`; never the secret |
| `ft reset` | `{"ok":true,"reset":"customer"}`, then the customer reset below (the factory namespace survives) and a restart |
| `ft reset --all` | `{"ok":true,"reset":"all"}`, the same plus the factory namespace (secret, pop, serial, test and bring-up records) |

## Button holds (volume buttons 1 + 4)

| hold | what |
|---|---|
| 5 s | a tone (two rising notes): "forget WiFi" is armed; letting go before 10 s forgets the network (stories stay), says `wifi_reset_done` and restarts into BLE provisioning |
| 10 s | a second tone (three falling notes): **customer reset**: best-effort `POST /api/hubs/{uuid}/unbind` with the device token (3 s, skipped offline), device token and its age deleted, card cache wiped (stories, manifests, download queue, uid mappings; not the prompt pack, not `/.device`), fault ring cleared, volume / volume ceiling / brightness settings deleted, WiFi forgotten, restart into provisioning. The factory namespace stays. The app/admin command `factory-reset` and `ft reset` run the same reset |

Buttons 2 + 3 held 5 s restart the toy (unchanged).

## Bench commands (`-DTT_BENCH` only)

| command | what |
|---|---|
| `endpoint set <api_base> <reverb_host> <port> <tls 0\|1> [key]` | point the unit at a bench portal (NVS; `restart` to apply); the captive-portal probe follows it (`http://<host>:<port>/storage/ping.json` for an http base) |
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
| `bringupreset` | clear the bring-up flag and re-run the gauge bring-up |
| `gaugebus release` | hand the battery bus to bqStudio / EV2400 (persists across reboots) |
| `gauge1s`, `gaugeprog`, `gaugeresetlearn`, `gaugelearned`, `gaugecalcurrent` | gauge provisioning tools |
| `dfread <hexaddr> [len]`, `dfwrite8 <hexaddr> <hexval>` | gauge data flash |
| `learndis`, `learnrest`, `learnchg` | gauge learning cycle (discharge stops at 2.90 V) |
| `sleeptest <min> [railon] [nfcawake] [hold]` | sleep current via the gauge; result prints on the wake boot |
| `chargemon [seconds]` | one charger/gauge line per second (max 300 s) |
| `pdvolt <5\|9\|12\|15\|20>` | request another USB-PD voltage |
| `crash` | `abort()` on purpose (rollback / crash-loop guard tests) |

In a production image these fall through to "Unknown command" (`endpoint set` says it is a
bench command; `gaugebus release` prints the bus status).

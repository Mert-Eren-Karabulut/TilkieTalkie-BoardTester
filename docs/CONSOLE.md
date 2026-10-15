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

Confirmations (`factory`, `delete`, `deletefig`, `poweroff`) wait at most 10 s for `yes`,
feeding the loop watchdog meanwhile; any other answer or silence cancels.

## Production commands

| group | commands |
|---|---|
| system | `help`, `version`, `faults`, `restart`, `config`, `debug`, `efusemac` / `hubid`, `heap`, `stack`, `timing`, `tasks`, `diag [on\|off]`, `coredump`, `coredumperase`, `endpoint`, `endpoint clear`, `ota`, `ota check [release_id]`, `ota url <url> <sha256> <size> [version]` |
| WiFi / Reverb | `qr`, `reset`, `stats`, `reverbstatus`, `reverbclean`, `reverbstart`, `send <message>`, `testauth` |
| battery / power | `battery`, `battscan`, `batterycfg`, `batteryclearflags`, `chargekick`, `gaugefets`, `gaugeprot`, `gaugecuvfix`, `gaugebus`, `gaugebus attach`, `pdstatus`, `power`, `poweron` |
| sleep | `sleep`, `sleep now`, `sleepafter <ms>`, `cancelsleep`, `sleepstatus` |
| files | `sdtree`, `sdinfo`, `sdformat`, `sdbench`, `sdwritebench [chunkKB] [count]`, `dlstats`, `dlqueue`, `dlclear`, `required`, `checkfiles`, `cleanup`, `download <url> <path>`, `addfile <path> <url>`, `deletefile <path>` |
| audio | `play <path>`, `play`, `pause`, `resume`, `stop`, `next`, `prev`, `playlist`, `track`, `volup`, `voldown`, `volume`, `volume <n>`, `audio`, `amp on\|off\|auto` |
| LED | `ledon <hex> <0-255>`, `ledoff`, `pulse <hex>`, `rapid <hex> <count>` |
| NFC | `nfcstatus`, `nfcdata`, `nfcreed` / `nfcpogo`, `nfcdiag`, `nfcsleep` |

`faults` prints the active faults and the recent fault events (docs/FAULTS.md).

## Bench commands (`-DTT_BENCH` only)

| command | what |
|---|---|
| `endpoint set <api_base> <reverb_host> <port> <tls 0\|1> [key]` | point the unit at a bench portal (NVS; `restart` to apply) |
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

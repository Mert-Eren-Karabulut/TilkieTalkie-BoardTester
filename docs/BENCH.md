# Bench setup

How the development boards are driven from this repo. Production units are never driven this way
(docs/MANUFACTURING.md); the console commands below exist in the bench build only.

## Builds and ports
- `pio run -e esp32-s3-devkitc-1` is the production image; `pio run -e bench -t upload` adds the
  bench console (`-DTT_BENCH`, docs/CONSOLE.md). Both share `sdkconfig.esp32-s3-devkitc-1`.
- The toy enumerates as `/dev/cu.usbmodemXXXX` (USB-Serial/JTAG); the bench unit has been
  `/dev/cu.usbmodem21201`. A VS Code "pio device monitor" holds the port: `lsof` it first.
- `tools/ttserial.py` is the scripted console (see tools/README.md); each `--wait` is the delay
  *before* its command.
- The calibration unit's serial header is separate from its USB-C power: polling continues on
  battery until the toy goes quiet, and the brick can be plugged or pulled independently.

## Power on the bench
- A USB power bank power-cycles the board (it cuts its output at low load) and once took the
  port away for minutes: unattended runs need a wall USB-C supply or the 9 V brick.
- The charger ignores VBUS while in HIZ (`learndis`), so USB can stay connected for logging
  during a discharge; `learnchg` lifts HIZ.
- Disabling the charger (EN_CHG off) sags VSYS to ~2.8 V on this board; the ESP browns out on
  boot inrush. The firmware keeps it enabled and uses HIZ instead.
- A gauge DEVICE_RESET opens both FETs for a moment: on battery the board power-cycles, on USB
  the full system usually browns out and reboots. Do gauge resets in monitor mode or on USB.

## Gauge learning cycle (bench build, docs/GAUGE-CHEMISTRY.md for the full record)
Charge to "Charge done" on the brick, rest 5 h at 0 mA (VOK appears only when a rest *ends*,
never wait for it at 0 mA), `learndis` (HIZ, LEDs full white, −420 to −480 mA, auto-sleep off),
pull the brick before the knee (about 3.05 V under load, ~8 h in: a brownout restart near the
end re-enables the charger), let the cell run to the floor on battery, rest, then recharge.
`gaugeimage inhibit` first if the unit starts from Update Status 04, or the boot replays the
golden image over the cycle. Read-outs: `battery`, `gaugeit`, `gaugeprot`, `gaugedump`.
`gaugelearned confirm` / `gaugeresetlearn confirm` wipe the learned state; never send them to a
unit with state worth keeping.

## Local portal
The portal repo (`/Users/mert/Herd/portal.tilkietalkie.com`, branch `develop`) has
`scripts/bench/bench.sh up|seed|check|device|status`: a LAN-bound copy of the portal on :8000
with Reverb on :8080 (plain ws, app key `ttbench`), its own SQLite and redis prefix, Herd and
`.env` untouched. `seed` creates admin `bench@tilkietalkie.local` / `bench1234`, hub
`250696234569532` (the bench board) and units `04-1F-76-D2-56-68-80` (Bench Fox, 5 tracks),
`04-BE-11-C2-00-00-01` (Bench Owl), `04-BE-11-C2-00-00-02` (Bench Wildcard, custom tracks).
`bench.sh device` prints the console command for the toy:
`endpoint set http://<ip>:8000/api <ip> 8080 0 ttbench` then `restart` (NVS-backed);
`endpoint clear` + `restart` returns it to production. A local copy of the factory area needs
`FACTORY_PASSWORD` in that `.env` and `DEBUGBAR_ENABLED=false` for the manual screenshots.

## Measuring
- Console output inflates loop-gap numbers; use the LED frame-gap metric (`diag on`).
- Reference wake timing on 1.2.10 (button wake): full system 4.4 s, first prompt 6.4 s, WiFi
  7.6 s, portal services 10.5 s, WebSocket subscribed 12.7 s.
- FastLED's RMT interrupt cannot run during flash writes, so the LEDs are driven directly over
  RMT with the whole frame preloaded; a flashing strip during an update is a driver problem,
  not the renderer.
- `sleeptest` uses the gauge's RemainingCapacity as the ammeter (a series meter in the cell path
  corrupts the gauge's impedance tracking).

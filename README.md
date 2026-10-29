# TilkieTalkie firmware (Adem v7 toy)

ESP32-S3 firmware for the TilkieTalkie story toy: NFC figures, audio playback from the SD card,
content sync and OTA from the portal, BLE WiFi provisioning, a BQ28Z610 fuel gauge and a BQ25792
charger on a single LiFePO4 cell. The production branch is `IDF-Migration`. The portal (Laravel)
lives in a separate repo (`/Users/mert/Herd/portal.tilkietalkie.com`).

## Build
PlatformIO with pioarduino (Arduino 3.3.x on ESP-IDF 5.5.5). `pio run -e esp32-s3-devkitc-1`
is the production image, `pio run -e bench -t upload` the bench image with the extra console
commands (`-DTT_BENCH`). The firmware version is `CONFIG_APP_PROJECT_VER` in
`sdkconfig.esp32-s3-devkitc-1`; releases are cut with `tools/release.sh` (docs/RELEASE.md).

## Layout
- `src/`: the firmware. Entry `main.cpp`; `Console` (serial console), `FactoryTest` (the `ft`
  station protocol), `Bringup` (first-boot gauge provisioning and calibration),
  `BatteryManagement` / `ChargerSupervisor` / `DeadCellRecovery` / `GaugeGoldenImage`
  (power), `OtaManager`, `RequestManager` / `FileManager` / `ReverbClient` (portal, content,
  WebSocket), `AudioController` / `ReadAheadFileSource` / `VoicePrompts` / `PromptPackManager`,
  `NfcController`, `LedController` / `UxScenes`, `SleepController`, `Faults`, `TimeSync`.
  `sleepprobe_main.cpp` and `bleprobe_main.cpp` are compiled only with their probe flags.
- `docs/`: PROTOCOL-V2 (device ↔ portal contract), OTA-STATE-MACHINE, CONTENT-SYNC, FAULTS,
  CONSOLE, FT-PROTOCOL and MANUFACTURING (the station), GAUGE-CHEMISTRY (the learned gauge
  image), VOICE-PROMPTS, HARDWARE (pins, power parts, protection model), BENCH, RELEASE,
  `schematic/`. `docs/gauge/` holds TI-licensed gauge data and dumps and is not in git.
- `SHIP-CHECKLIST.md`: the running log of releases and bench findings.
- `tools/`: release and USB-image scripts, SD card preparation, prompt generator, fault catalogue
  export, gauge image generator, serial bench scripts (tools/README.md).
- `datasheets/`: chip PDFs and text dumps. `sdcard/sounds/`: the current prompt pack.
- `default_16MB.csv`: partition table. `CMakeLists.txt`, `dependencies.lock`: ESP-IDF glue.
- Ignored: `releases/` (built images), `backups/`, `.pio/`, `managed_components/`.

# Tools

Python scripts run with the PlatformIO environment (it has pyserial):
`~/.platformio/penv/bin/python tools/<script>`.

- `release.sh <ver>`: bump the version, build the production image, drop it in `releases/` with
  its sha256 (docs/RELEASE.md). `usb-image.sh <ver>`: the flash-at-0x0 image for the station.
- `sdcard-prepare.sh`: lay the prompt pack out on an SD card. `sounds/gen_prompts.py`: generate
  the prompt pack (docs/VOICE-PROMPTS.md).
- `faults.py --out <json>`: export the fault catalogue for the portal (docs/FAULTS.md).
- `gauge-image.py <dump> --chem-id <hex>`: turn a `gaugedump` of a learned gauge into
  `src/GaugeGoldenImage.{h,cpp}` (docs/GAUGE-CHEMISTRY.md).
- `ttserial.py --port <port> [--reset] [--quiet-boot] --cmd <c> --wait <s> ... --timeout <s>
  [--log <file>]`: scripted serial console; each `--wait` is the delay *before* its command,
  survives USB re-enumeration, `--quiet-boot` hides bootloader noise. A VS Code serial monitor
  holds the port (`lsof /dev/cu.usbmodem21201`).
- `batt_series.py <log>`: tabulates repeated `battery` dumps (cell V, current, BAT/PACK drop).
- `latency.py <log>`: first-response latency per sent command.

Console commands: docs/CONSOLE.md. Bench setup and the local portal: docs/BENCH.md.

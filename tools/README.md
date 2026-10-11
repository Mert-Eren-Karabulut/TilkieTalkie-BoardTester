# Bench tools

All scripts use the PlatformIO Python environment (it has pyserial):
`~/.platformio/penv/bin/python -I tools/ttserial.py ...`

- `ttserial.py` — scripted serial console for the device (ESP32-S3 USB-Serial/JTAG, usually
  `/dev/cu.usbmodem21201`). Sends `--cmd` strings after `--wait` seconds, prints timestamped
  lines, survives USB re-enumeration, `--reset` resets the chip first, `--quiet-boot` hides
  bootloader noise. Example:
  `ttserial.py --port /dev/cu.usbmodem21201 --reset --timeout 60 --quiet-boot --cmd battery --wait 20 --cmd timing --wait 5`
  Note: a VS Code "pio device monitor" holds the port; check `lsof /dev/cu.usbmodem21201`.
- `batt_series.py <log>` — tabulates repeated `battery` dumps (cell V, current, BAT/PACK drop, path resistance).
- `latency.py <log>` — first-response latency per sent command (loop responsiveness).

Useful firmware console commands added for the bench: `timing`, `tasks`, `heap`, `coredump`,
`dfread <hexaddr> [len]`, `dfwrite8 <hexaddr> <hexval>`, `sdwritebench [chunkKB] [count]`,
`cat <path>`, `tap <uid>` / `untap` (simulated figure), `download <url> <path>`.

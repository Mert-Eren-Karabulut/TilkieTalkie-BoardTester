# Ship checklist (firmware) — 2026-10-12

Status of the firmware on `IDF-Migration` against "ready to put on the shelves". Narrative in
NIGHT-REPORT-2026-10-11.md, plan in PLAN.md.

## Verified on the bench
- Boot, WiFi, token over HTTPS (CA bundle), Reverb wss + `private-device.{uuid}`, reports, commands.
- Tap → manifest → downloads (resume, sha256) → play as tracks arrive; re-tap cache hit; figure change restarts.
- Audio: pause/resume/next/prev/seek, 15 track changes without leaks, playback through 3 s SD stalls, download during play.
- OTA: update, resume, confirmation after 60 s, bootloader rollback, crash-loop guard, status reports.
- Power: no sleep on USB, charger input budget for weak sources, gauge keeps its FET closed at full, charge terminates.
- Memory: idle internal heap ~68 KB (NimBLE, decoder in PSRAM); tap storm with 4 downloads survives.

## Remaining before ship (firmware)
1. Manifest fetch runs on the loop task (up to 8 s stall on a dead server) → request worker task.
2. Time to first story: play the first track while it downloads (streaming from the partial file).
3. `CONFIG_SPI_FLASH_AUTO_SUSPEND` so LED/WebSocket keep running during OTA flash writes.
4. Release build hygiene: log level, no periodic diagnostics, console stays for service.
5. Deep sleep + wake test (needs a hand: unplug the charge port, `sleep`, press a button); sleep current unknown.
6. BLE provisioning on NimBLE (needs the mobile app once).
7. Low-battery path: forced sleep at the critical threshold uses the coulomb-counted SOC — re-check after calibration.

## Needs hardware / owner
- Gauge: LiFePO4 chemistry + learning cycle in bqStudio (EV2400). Until then SOC is coulomb-counted.
- Next board spin: ~0.3 Ω cell-to-charger path; VSYS must not depend on the battery path on USB.
- SD card quality (bench card stalls 3 s per 32 KB).

## Factory mode — next phase (plan: docs/FACTORY-MODE.md)

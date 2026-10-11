# Ship checklist (firmware) — 2026-10-11, evening

Status of the firmware on `IDF-Migration` (release 1.2.0, code 10200, on the bench board
against production) against "ready to put on the shelves". Narrative in
NIGHT-REPORT-2026-10-11.md, plan in PLAN.md, factory mode in docs/FACTORY-MODE.md.

## Verified on the bench
- Boot, WiFi, token over HTTPS (CA bundle), Reverb wss + `private-device.{uuid}`, reports, commands.
- Tap → manifest → downloads (resume, sha256) → play as tracks arrive; re-tap cache hit; figure change restarts.
- Manifest fetch, OTA check and OTA status posts run on a worker task: a slow or dead portal
  no longer stalls the loop (was up to 8 s; against production nginx it was 65 s on every OTA
  check because a 204 has no body — fixed).
- Audio: pause/resume/next/prev/seek, 15 track changes without leaks, playback through 3 s SD stalls, download during play.
- OTA against production: check (204 in 0.1 s), update, resume, confirmation after 60 s,
  bootloader rollback, crash-loop guard, status reports (incl. the first post after an idle
  keep-alive connection, which nginx closes after 65 s — retried on a fresh connection).
- Power: no sleep on USB, charger input budget for weak sources, gauge keeps its FET closed at full, charge terminates.
- Memory: idle internal heap ~62 KB with the production TLS WebSocket, min ~16 KB during a tap
  (TLS fetch + downloads + first I2S start); manifests and file lists parse into PSRAM.
- LED task: 20 ms frames, max gap 21–28 ms in normal use (new Diag metric); ~300 ms freezes only
  during OTA flash block erases (cache off; flash auto-suspend tested, no gain, left off).

## Added on the night of day 2 (1.2.2)
- Network services start whenever WiFi comes up (fresh provisioning, router back), not only at boot.
- LED: the charge bar is the idle picture only; pairing/connecting/loading/downloading/playing/OTA/error win.
- Voice prompts + tones (Turkish) from the SD card, docs/VOICE-PROMPTS.md; the card needs the pack
  (`tools/sdcard-prepare.sh /dev/diskN`). Fruit error codes in `device_report.last_error` (portal: not stored yet).
- Volume buttons 5 s = forget WiFi; 2+3 5 s = restart.

## Remaining before ship (firmware)
0. Out-of-box run #2 with the prompts and the pack on the card (pairing, update, first figure, buttons, sleep).
1. Deep sleep + wake test (needs a hand: unplug the charge port, `sleep`, press a button); sleep current unknown.
2. BLE provisioning on NimBLE (needs the mobile app once).
3. Low-battery path: forced sleep at the critical threshold uses the coulomb-counted SOC — re-check after gauge calibration.
4. Time to first story on a never-tapped figure = first track download (SD-bound, ~2.4 Mbps:
   ~10 s for a 3 MB track). Streaming the first track while it downloads would cut that to ~2 s;
   deferred (touches FileManager, ReadAheadFileSource, AudioController; design: reader re-opens
   the growing `.tmp` after the writer's periodic flush, EOF before Content-Length = wait).
5. Volume policy for production (bench runs at 15 % with a 20 % ceiling).

## Needs hardware / owner
- Gauge: LiFePO4 chemistry + learning cycle in bqStudio (EV2400). Until then SOC is coulomb-counted.
- Next board spin: ~0.3 Ω cell-to-charger path; VSYS must not depend on the battery path on USB.
- SD card quality (bench card: 0.3 MB/s sustained writes; it bounds download speed).
- Asset downloads are plain HTTP (AsyncTCP, no TLS) verified by the sha256 from the TLS
  manifest. nginx must keep serving `/storage` over port 80 (no forced HTTPS redirect).
- Listening check of the audio path, LED patterns, button mapping.

## Factory mode — next phase
Plan with open decisions: docs/FACTORY-MODE.md.

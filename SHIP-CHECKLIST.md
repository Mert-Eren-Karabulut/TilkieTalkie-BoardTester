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
- LEDs driven directly over RMT with the whole frame preloaded (no flashing during flash writes).
- SD writes off the loop (maintenance task); free space counted from the FAT, reported in MB.
- Portal pushes `ota-update` on upload; device checks hourly; dock heartbeat 15 s.
- **The bench card has a 127 MB FAT volume on a 30 GB card**: reformat with `tools/sdcard-prepare.sh`.
  Production cards must be formatted full-size by the same script (factory step).

## Added 2026-10-15 (1.2.11 – 1.2.14)
- Portal hub page: live status, stories on the card (`list-files`, `delete-figure`, `delete-track`),
  3 s reports while the page is open; OTA only on the dock; OTA state in the report.
- Content sync (docs/CONTENT-SYNC.md): `sync-figure` push from the portal on content changes,
  `If-None-Match` re-check of every cached unit after boot and every 12 h, replaced files re-downloaded.
- Playlist waits for the next track when the figure is still downloading (prompt `download_wait`,
  needs the new pack on the card) instead of ending after the cached ones.
- Interrupted figure downloads are recovered after a reboot; inventory lists downloading figures.
- **Not yet run on hardware**: everything from 1.2.11 on (bench board away). First things to try:
  tap a new figure with 3+ tracks and let track 1 end before track 2 is down; delete a figure
  from the hub page and re-tap; change a track on the portal and watch the toy re-download.

## Remaining before ship (firmware)
0. Out-of-box run #2 with the prompts and the pack on the card (pairing, update, first figure, buttons, sleep).
1. Deep sleep on battery and wake by button verified 2026-10-12 (sleep after 5 min idle; wake: LEDs at once, hello at 6.5 s, portal at ~11 s). Still open: sleep current (`sleeptest`).
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

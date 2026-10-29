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
- Prompt packs (1.2.15, docs/VOICE-PROMPTS.md "Remote update"): the portal pairs a pack with each release,
  the device stages it before an update and activates it on the new firmware's first boot; rollback-safe.
  First use on an existing device: its card has no pack.json → it downloads the pack once (~1 MB) and swaps it in.
- 1.2.16 (from the colleague's device, evening of 2026-10-15): playlist continued from the first
  track instead of the next one after a download wait (the wait prompt was taken for the current
  story track); WiFi that never reconnected for 16 min next to a working router (now: radio reset
  every 4th failed attempt, idle restart after 15 min offline, radio reset when the link is up but
  the portal unreachable 10 min); loop watchdog 120 s (a hung loop used to need a battery pull);
  a token check without an answer no longer rotates the hub token.
- 1.2.17: the WiFi recovery and the wifi_lost prompt apply only while the stored network is in range
  (periodic scan for the SSID). Away from home the toy stays quiet, plays from the card and looks for
  the network every 2 minutes; no radio resets, no restarts.
- 1.2.18: fault codes (docs/FAULTS.md) end to end; the audit's safe fixes (docs/AUDIT-2026-10-16.md):
  console in Console.cpp with bench commands behind -DTT_BENCH (env:bench, never shipped), main.cpp
  split into modules, download give-ups complete the figure, batched queue writes, races closed, audio
  lock discipline and the volume-beep bug, skips for bad tracks, OTA check timeout, prompt pack
  power-loss safety, NFC 10-byte UIDs, WiFi credential writes only on change, tagged logging.
- **Bench-validated 2026-10-16 on 1.2.18/1.2.19**: boot, WiFi, token, Reverb, fault events to the portal,
  prompt pack staging + activation, OTA 1.2.18 -> 1.2.19 over the air (offer, prompt, download, activate,
  reboot, confirm), figure tap -> download -> play, delete from the hub page -> re-tap -> re-download, 3-track
  playback. Not exercised on the bench (link too fast): the download wait + volume beep path (desk-reviewed).
  1.2.19 is on beta; promote to stable for the field unit. First things to try:
  tap a new figure with 3+ tracks and let track 1 end before track 2 is down; delete a figure
  from the hub page and re-tap; change a track on the portal and watch the toy re-download.

## Remaining before ship (firmware)
0. Out-of-box run #2 with the prompts and the pack on the card (pairing, update, first figure, buttons, sleep).
1. Deep sleep on battery and wake by button verified 2026-10-12 (sleep after 5 min idle; wake: LEDs at once, hello at 6.5 s, portal at ~11 s). Still open: sleep current (`sleeptest`).
2. BLE provisioning on NimBLE (needs the mobile app once).
3. Low-battery path: forced sleep at the critical threshold voltage-based (3.00 V debounced), not the state of charge.
4. Time to first story on a never-tapped figure = first track download (SD-bound, ~2.4 Mbps:
   ~10 s for a 3 MB track). Streaming the first track while it downloads would cut that to ~2 s;
   deferred (touches FileManager, ReadAheadFileSource, AudioController; design: reader re-opens
   the growing `.tmp` after the writer's periodic flush, EOF before Content-Length = wait).
5. Volume policy for production (bench runs at 15 % with a 20 % ceiling).

## Needs hardware / owner
- NFC board routing error (first batch of NFC boards): the PN532 module's regulator output was
  tied to the board's supply, two feeds colliding; the module worked but ran hot and the
  back-fed regulator drew current in sleep, draining the cell. Corrected on the bench units;
  the unit shipped to the app developer (2026-10) has the old board. Consequence seen there:
  cell run down past the 3.0 V cutoff (gauge FETs open) and then below the gauge's own
  shutdown (2.3 V), so on USB the charger sees no battery (STAT LED on with a short blink
  every 1-2 s) and nothing can close the FETs. Recovery only by charging the cell directly.
  Firmware follow-up done in 1.3.13 (below): the cutoff/ship state keeps the CHG FET closed
  (DSG off only) so a charger always reaches the cell while the gauge is alive, and a unit
  without a gauge answer stays awake with charging off (fault 115) instead of sleeping into
  the dead-cell hold. Next spin: feed the gauge's REGIN from the charger side as well
  (diode-OR) so a dead cell does not kill the gauge.
- Gauge: LiFePO4 chemistry + learning cycle in bqStudio (EV2400).
- Next board spin: ~0.3 Ω cell-to-charger path; VSYS must not depend on the battery path on USB.
- SD card quality (bench card: 0.3 MB/s sustained writes; it bounds download speed).
- Asset downloads are plain HTTP (AsyncTCP, no TLS) verified by the sha256 from the TLS
  manifest. nginx must keep serving `/storage` over port 80 (no forced HTTPS redirect).
- Listening check of the audio path, LED patterns, button mapping.

## Factory mode — next phase
Plan with open decisions: docs/FACTORY-MODE.md.

## 1.3.0 (protocol v2, 2026-10-23) — not yet run on hardware
Built and merged from two reviewed branches (docs/PROTOCOL-V2.md, docs/OTA-STATE-MACHINE.md); nothing
in this release has been on a device yet. Bench plan, in order, against the LAN bench portal first
(`scripts/bench/bench.sh up` in the portal repo; `endpoint set` on the device), then production:
1. USB-flash `releases/tilkietalkie-1.3.0-usb.bin` on a card that has the old pack; boot, WiFi, probe
   (`/storage/ping.json`), token (legacy GET: the bench hub has no secret), Reverb, hub page live.
2. Prompt pack c1f1888c0884: upload on the portal, pair with the release; live sync must bring the
   two new prompts (`not_linked`, `setup_captive`) to the card.
3. Enrollment: `POST /api/factory/hubs` for the bench uuid on the portal, `ft enroll <secret> <pop>`,
   restart: token now comes from `POST /token` + X-Hub-Secret; hub page shows "enrolled".
4. `GET /api/hubs/{uuid}` link check: unbind the hub on the portal → fault 110 + `not_linked` prompt
   after the hello; bind with the pairing code → fault clears.
5. Customer reset: volume buttons 10 s (tone at 5 s, second tone at 10 s) → WiFi + token + cache gone,
   portal shows the hub unbound; re-provision over BLE with the per-unit pop (needs the app).
6. Captive portal: a phone hotspot with a captive page, or a LAN rule that answers HTTP with HTML →
   during pairing the toy says `setup_captive` and forgets the network; already provisioned → offline
   only. A network without internet must pair normally and stay offline.
7. OTA 1.3.0 → 1.3.1 (any trivial bump) dock-only: offer while undocked waits; dock → prompts → download;
   undock mid-download continues; 10-min-old offer re-checks; rollback test with `TT_OTA_CRASH_TEST` →
   next offer declined (`declined` event on the portal); explicit pin overrides.
8. Range resume: pull the WiFi mid-download, reconnect → the `.tmp` continues (206 in the portal log).
9. Revoked card: delete the card on the portal → tap → 410 → cache purged, "figure unknown".
10. Foreign card: card from another uuid → cache wiped once (fault 306), prompt pack kept.
11. Faults page: events carry `at` (SNTP) and `approx`; the report carries `fw` and `prompts`.
12. Inventory with a 60+-track figure: parts merged on the hub page, `truncated` shown.

**Bench results 2026-10-23 (LAN bench portal, bench board):** 1 boot/probe/token/Reverb OK;
2 pack c1f1888c0884 live-synced and activated (28 files, sha verified); 3 `ft enroll` → `POST /token`
device token (90 d), hub page enrolled; 4 unbind → fault 110 + `not_linked` spoken after the hello,
bind → cleared; 7 OTA 1.3.2→1.3.3 end to end with "pending verification" and confirmation on Reverb,
crash-test image rolled back by the bootloader, blacklisted, portal stopped offering it; 11 events
carry exact `at` times, the report updates firmware and pack on the hub page. Fixed on the way:
the post-update verdict (a bootloader without rollback made a good update report `failed`), the
legacy token path hammering a 410, out-of-order progress posts, prompt state "unknown" on boot.
Not run: 5 customer reset and 6 captive portal (need the app / a captive network), 8 Range
resume, 9 revoked card, 10 foreign card, 12 60-track inventory (need a figure on the pins / a
second card). The production build has no `endpoint set`: LAN tests use env:bench.

## 1.3.6: factory test protocol (`ft`, docs/FT-PROTOCOL.md) — bench 2026-10-23
Driven over the console as the station does it: `ft begin` (identity, bring-up state, card, pack,
battery, tested record), battery / pd / sd automatic tests with data, leds and speaker with the
operator question and `ft ok`, ble refused cleanly on a provisioned unit, wifi failure path (20 s,
stored home network restored afterwards), buttons + `ft abort`, `ft result`, `ft commit`,
`ft end` (log level back), `ft show` (label). Not run: buttons/pogo/nfc/jack (need hands), wifi
success path (needs the factory network), bring-up re-run, `ft ship` (unplug within 10 s, wake by
USB), the station page itself in Chrome (WebSerial + ESP Web Tools flash).

## 1.3.8: fresh (erased) units boot-looped at pairing — fixed (2026-10-24 night)
The station's flash with erase put the bench unit into an unprovisioned boot for the first time
on this toolchain, and it crashed inside the BLE controller init on every boot. Root cause:
Arduino core 3.3.7+ frees the BLE controller memory in initArduino() unless the sketch declares
Bluetooth in use (only the core's own BLE library does); this firmware drives BLE through the
IDF provisioning manager, so the controller later initialised into heap memory and corrupted it.
Fix: `extern "C" bool btInUse(void) { return true; }` in main.cpp (provisioned boots still release
the memory themselves). Verified: an erased unit boots, advertises as TilkieTalkie_<uuid>, `ft begin`
reports ble_ready; the probe build (`-DBLE_PROBE_BUILD`, src/bleprobe_main.cpp) isolates the BLE
controller for future toolchain checks. Also in 1.3.8: the SD download bounce buffer is taken when
the network comes up instead of at boot, and a memory guard (fault 114) refuses to start BLE
instead of crashing. Pairing with the app on a fresh unit is still to be done by the owner.

## 1.3.9: ship mode works (2026-10-24)
Adem v7 has no ship FET on the BQ25792 SDRV pin (a 1 nF capacitor only), so the charger's ship
mode never cut the cell: after an unplug the unit kept running on the battery. Ship mode now
clears the gauge's FET_EN (BQ28Z610 protection FETs open) the moment VBUS is gone: verified on
the bench with the station: dark at once after the unplug, no reaction to buttons, wake on USB,
gauge FETs closed again by the boot path. The low-battery cutoff uses the same path. Shelf
drain in that state = the gauge in SLEEP (~100 µA per the BQ28Z610-R1 datasheet) plus the cell's
self-discharge; the gauge's SHUTDOWN mode (~0.5 µA, wake on charger) is the next step if a
longer shelf life is needed.


## 1.3.11: pack-resistance factory test, USB attach record (2026-10-25)
- `ft test pack` (station step "Battery pack resistance", between USB PD and SD card): charger
  input HIZ with the LEDs dark for 8 s, then all LEDs full white for 8 s, R = ΔV/ΔI from the
  gauge; fails above 100 mΩ (provisional). The bench pack that broke the learning cycles reads
  341 mΩ; a sound 26700 LFP pack is expected at 20-60. Set the final limit from the first
  known-good units (their `r_mohm` is in each factory record).
- Fault 509 USB_SOURCE: every USB attach records the PD contract (`pd <mV> <mA>`) or the plain
  5 V outcome (`ch224 … no-pd` / `no-ch224 …`), for charger interop diagnosis after the fact.
- Bench only: `learndis` with the battery bus released, `gaugedump` / `gaugemac` / `gaugeit`.
- On the portal: release 1.3.11 on beta with its USB image; flash page 1.3.11.
- Not in this release: the gauge golden image (the learning cycle restarts after the pack is
  fixed, docs/GAUGE-CHEMISTRY.md 2026-10-25), SOC from the gauge instead of the coulomb count.

## 1.3.12: pack-test limit 450 mΩ (2026-10-26)
The production cell type itself reads 320-340 mΩ on the 8-s load step (meter-verified at the
cell terminals, docs/GAUGE-CHEMISTRY.md 2026-10-26), so the station's limit now catches a bad
contact on top of the cell, not the cell. Decision: keep these cells (capacity is good); the
gauge is configured for them instead (voltage calibration, Load Select = present average
current, one learning cycle), see docs/GAUGE-CHEMISTRY.md.

## 1.3.13: cutoff / ship keeps the charge path, no-gauge boot stays awake (2026-10-27, bench)
Why: the app developer's unit (NFC back-feed) went through the 3.0 V cutoff, which cleared the
gauge's FET_EN and opened both FETs; the isolated cell sank below the gauge's shutdown and the
charger then saw no battery at all, nothing could ever close the FETs again (software could not
recover it: cell charged directly). Now:
- `openGaugeFets()` (low-battery cutoff, `ft ship`) raises the gauge's CUV threshold in data
  flash (0x46B3, 2500 → 3700 mV) so the gauge trips its own under-voltage protection: DSG FET
  open, CHG FET closed. If CUV has not tripped after 3 s (seen: the gauge kept the old
  threshold for 14 s while idle) a DEVICE_RESET makes it re-read the flash and trip. On battery
  the unit dies there (a brief second boot is possible inside the reset window; a boot without
  USB leaves the cut alone).
- Boot on USB: `restoreGaugeFetControl(usb)` writes 2500 mV back and runs the reconnect ladder
  `reconnectCutCellNow()`: cell above 3.05 V → charger paused (CE released: charging into the
  open BAT node of a full cell collapses the rail, brownout loop on the bench), Protection
  Configuration CUV_RECOV_CHG released for a moment so CUV clears on voltage, latch back on;
  else a gauge reset. Cell below 3.05 V → charging on, the gauge clears CUV itself once the cell
  is above 3.0 V with current flowing (monitor loop retries every 10 s). CE is now asserted
  only after the gauge work in `applyPowerConfiguration()`.
- Monitor mode gate: a cut cell (`isCellCut()`: CUV in SafetyStatus or XDSG) never starts the
  full system (it reads a fine voltage but cannot buffer WiFi).
- No gauge answer on USB: charger held off (CE high), the dead-cell hold is skipped, fault 115
  GAUGE_ABSENT, unit awake and reachable by the station. Except for a 300 ms charger pulse at
  boot and every 30 s: a BQ28Z610 in SHUTDOWN (cell pulled and put back, or sunk below its
  shutdown voltage and charged a little) never comes back on the cell alone, it needs a voltage
  on its PACK pin, which only the charger can put there (TI E2E; bench 2026-10-28: after the
  battery was re-plugged on USB the gauge stayed silent for minutes with the charger off, and
  answered 2 s after the first pulse). The rescan then re-applies the power configuration and
  charging is on again.
- Data-flash writes hold the bus lock for the whole block + checksum + read-back (a poll in
  between made the write "fail" / read back 0).
Bench 2026-10-27 (demo unit, USB): cut → CUV 0x1, XDSG, FETs On/Off, 0 mA → the full system
power-cycled on the USB-only rail → boot: threshold 3700 → 2500, "CUV latched (cell 3537 mV) -
charger paused", "CUV cleared on voltage", FETs On/On, charging, full system 2 s later, no
brownout.
Bench 2026-10-28 with the operator: `ft ship` on battery → "VBUS gone - ship mode now", dark
~3 s after the unplug (no flicker seen), USB back → threshold restored, "CUV cleared on
voltage", 75 mA charging, full system. Battery pulled on USB → the unit power-cycled (rail),
booted with "no gauge", fault 115, stayed reachable (MONITOR, charging off); battery back →
gauge silent until the wake pulse was added → then present 2 s after the pulse, FETs On/On,
VBAT present, full system 10 s later.

## Learned gauge image replayed at boot (2026-10-29, bench)
`src/GaugeGoldenImage.{h,cpp}` (generated by tools/gauge-image.py from the learned dump, 0x4100-
0x47FF) is written onto every gauge that still has the stock chemistry or Update Status 04, at the
first boot on USB, then verified (docs/GAUGE-CHEMISTRY.md). Demo unit: 0x1210/04 → 0438/06/Qmax
4443 in 7 s. Fleet effect once released: a unit on this firmware gets the image at its next boot
on USB (one gauge reset, ~7 s, charging continues). Not yet released.

## State of charge from the gauge (2026-10-29, bench)
The percentage is now the gauge's RelativeStateOfCharge and nothing else: the coulomb counter
anchored on an LFP voltage curve, the voltage-to-percentage table, the 10-sample voltage
smoothing and the "gauge SOC trusted" switch (all written before the gauge was calibrated) are
gone. `hasStateOfCharge()` is true while the gauge answers; the low-battery LED and voice
steps, the OTA 15 % abort, `ft ship` ("no gauge") and the charger's full-hold check gate on it,
the last value stays readable for the bar and the portal report. Without a gauge the voltage
still comes from the charger's ADC (cutoff and start gates are voltage-based). Verified on the
demo unit: 77 % = 3316 / 4319 mAh, `ft begin` soc 77. Not yet released.

## Gauge learned: Update Status 06 (2026-10-29)
Cycle 6 on the calibration unit (docs/GAUGE-CHEMISTRY.md 2026-10-29): Qmax 4443 mAh, Ra table
139–219 mΩ on the plateau, TrueFCC 4317 mAh. Golden image dumped (docs/gauge/dumps/, gitignored).
Open: replay the image at bring-up (chemistry 0438 + Qmax + Ra + Update Status 06) so every
produced unit starts learned, then SOC from the gauge instead of the coulomb counter; bqStudio
export for the record.

## 1.3.13 promoted to stable (2026-10-28)
Published on beta and promoted by Mert from his terminal (the auto-mode classifier blocks
production deploys/reads for the assistant). Flash page manifest 1.3.13 (portal 4f654b0).
Behaviour to tell the line: a unit whose battery is pulled on USB goes dark for a moment, comes
back without a gauge (fault 115, charging off) and, once the battery is back, wakes on the next
charger pulse, up to 30 s later. Factory password on production set by Mert (zsh `read -s
'FP?...'` form; the bash `read -p` form left it empty once, which only disables the login).

## 1.3.12 promoted to stable (2026-10-27)
Station flashing (esptool-js in the page) verified on the demo unit after the post-write reset
fix (portal b176c7f). Release 22 (1.3.12, pack #2 Yelda V2, rollout 100 %, not mandatory) is
the stable release; 1.3.5-1.3.11 stay on beta. Not in it: the gauge golden image (chemistry
0438 + learned Qmax/Ra) and SOC from the gauge; units produced now carry the stock chemistry
0x1210 until a later release replays the image at boot.

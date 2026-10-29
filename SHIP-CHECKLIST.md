# Ship checklist (firmware): the running log

Status of the firmware on `IDF-Migration` against "ready to put on the shelves", newest
sections at the end. Current stable release: 1.3.13 (2026-10-28). The station and the factory
flow: docs/MANUFACTURING.md; releases: docs/RELEASE.md; the board: docs/HARDWARE.md.

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
- 1.2.18: fault codes (docs/FAULTS.md) end to end; the audit's safe fixes (audit of 2026-10-16):
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
- Gauge: learned on 2026-10-29 (Update Status 06) and replayed onto every unit by the firmware;
  the percentage comes from the gauge. Done.
- Next board spin: docs/HARDWARE.md "Observations"; VSYS must not depend on the battery path on
  USB. (The ~0.3 Ω seen in October is the cells themselves, not the path.)
- SD card quality (bench card: 0.3 MB/s sustained writes; it bounds download speed).
- Asset downloads are plain HTTP (AsyncTCP, no TLS) verified by the sha256 from the TLS
  manifest. nginx must keep serving `/storage` over port 80 (no forced HTTPS redirect).
- Listening check of the audio path, LED patterns, button mapping.

## Remaining (collected from the 2026-10 audits, unverified unless noted)
- Release hygiene: `build_type = debug` and `CONFIG_COMPILER_OPTIMIZATION_DEBUG` (-Og) are still
  set; log levels; `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` not set.
- No secure boot / flash encryption: the device token and the WiFi password are readable over USB.
- Pogo pin in the ext1 wake mask: a figure left on the pins after the playlist → sleep → wake →
  autoplay loop (`SleepController.cpp`).
- NFC: finite passive-activation retries and RF off after a read (PN532 field left on).
- A card removed at runtime needs a reboot (no hot-swap); `uid_mappings` is one unbounded JSON
  string in a 20 KB NVS partition.
- Battery: a frozen gauge should fall back to the charger's VBAT so the 3.0 V cutoff still fires;
  one bus lock across charger/gauge read-modify-write (REG0F race); 1 A applied at a dead-cell
  wake before the hold drops it to 300 mA.
- Code shape: AudioController / BatteryManagement / RequestManager are large; splits pending.
- Portal: MySQL instead of SQLite, `APP_DEBUG=false`, off-box backups, `REVERB_ALLOWED_ORIGINS`.

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
Both follow-ups landed the same day (sections above): the image is replayed by the firmware
and the percentage is the gauge's; the bqStudio export (gg.csv + srec) is in docs/gauge/.

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

## 1.3.14 on beta, then stable (2026-10-29 morning)
Contents: the learned gauge image replayed onto stock gauges at the first boot on USB
(GaugeGoldenImage), the battery percentage from the gauge's RelativeStateOfCharge (coulomb
counter and voltage curve removed), `gaugelearned` / `gaugeresetlearn` behind `confirm`, the
repo cleanup (docs/HARDWARE.md, docs/BENCH.md, docs/RELEASE.md, README.md). Images in
`releases/` (app sha 61cb0b3c…, USB image sha bf9666b2…). Verified on the demo unit (bench
build of the same tree): image replay 0x1210/04 → 0438/06 in 7 s, percentage 77 % = 3316 /
4319 mAh. Fleet effect: every unit on this firmware gets the image once at its next boot on USB
(one gauge reset, ~7 s) and reports the gauge's percentage from then on. Published on beta
(release with pack #2 Yelda V2, USB image sha bf9666b2…, flash page manifest 1.3.14, portal
091e3fd); the demo unit runs the same production image from USB. Promoted to stable the same
morning on Mert's word (rollout 100 %, not mandatory): hubs on stable take it at their next
check and replay the gauge image at their next boot on USB.

## Readiness pass: failure feedback and recovery (2026-10-29/30, towards 1.3.15)
A read-only map of every failure path (fault code, LED, voice, recovery) found the mechanisms in
place but the plumbing between them broken. Fixed in three batches (2d12427, d111c54):
- Voice: a fault's prompt is spoken on every raise (the 10-min log dedupe used to mute repeats:
  a second empty figure, a second wrong password, and `download_failed` was never heard);
  `battery_low` and `nfc_read_trouble` were unreachable (no fruit word, gated on it); a
  postponed OTA (unplugged / under 15 %) is no longer "update failed"; 206 says "no stories".
- LEDs: a critical fault is red while docked too (the charge bar won before); the green "ready"
  flash only when the boot succeeded; a rejected WiFi password shows dim amber, not an off toy.
- Fault bookkeeping: a raise before `Faults::begin()` no longer wipes the stored events (115,
  606); a critical fault evicts the oldest lesser one from the full active set; BATTERY_LOW,
  TOKEN_REJECTED, PORTAL_UNREACHABLE, WS_AUTH_FAILED, OTA_CHECK_FAILED, PROMPT_FILE_MISSING clear
  when their condition ends; WiFi-task raises go through the loop; the cutoff no longer persists
  LED brightness 255; one missed gauge poll no longer invalidates the percentage.
- Parts that fail while running: SD card probed every 5 s (and after a read error) → 307 red,
  playback stopped, remount every 5 s, prompts reloaded when it is back (301 from boot clears the
  same way); NFC reader probed every 10 s idle → 701 (now critical, red) until it answers, boot
  failure retried every 10 s, a figure on a dead reader still gets the read-trouble prompt; the
  codec is retried every 10 s. `gSystemError` is gone.
- Sleep and buttons: pins already high at sleep time (figure left on, floating pogo line without
  the NFC board, stuck button) leave the wake mask; buttons held at power-up are ignored until
  released; pairing holds the 5-min sleep (bounded to 20 min).
Bench so far: `fault 501/701/204/301` from the console speak and list as expected; the unplug
tests (card out/in, NFC board out/in with a figure on the pins) are the acceptance test for the
recovery paths. Still open from the map: Armut ("not connected") is spoken for portal 4xx/5xx
while online (a new prompt would need a new pack); a card full (303) is silent; 502-507 are never
raised; no card-detect pin, so "card out" is noticed by the 5 s probe. Console: `fault <code>`.

## 1.3.15 on beta with voice pack faefcbd12031, superseded by 1.3.16 (2026-10-29 night)
Fault feedback and recovery (the readiness pass above) plus the fruit-code prompts per failure
category (docs/VOICE-PROMPTS.md "Error codes"): nfc_fault Çilek, battery_fault Ananas,
account_fault Limon, portal_fault Şeftali, storage_full Erik, audio_fault Mandalina; not_linked
says Kivi; a critical fault repeats its sentence every 10 min. Images in `releases/` (app sha
367fc691…, USB sha 877949ed…, pack `prompts-faefcbd12031.zip`). The publish (scp + `tilkie:
prompt-pack` + `tilkie:firmware-release`, docs/RELEASE.md step 2) is run by Mert; the demo unit's
card needs the six new files copied in (`sdcard/sounds/tr/*.mp3` + `pack.json` over `/sounds/`)
or a fresh card (`tools/sdcard-prepare.sh /dev/diskN releases/prompts-faefcbd12031.zip`).
Bench: `fault 701 / 504 / 107` reach nfc_fault / battery_fault / account_fault (files missing on
the old card, 404 reported with missing=6). The unplug tests: SD card out/in passed on batch 2;
NFC board out turned red (the board carries the pogo pins, so "figure on a dead reader" cannot
be tested on this hardware).
Mert published 1.3.15 (release 25) and pack 3 "Yelda V3 (fault codes)"; the pack was still
converting when the release was created, so it was paired afterwards (tinker). The demo unit's
card got the new files by copy; tests with Mert: card out → red, back → cleared; NFC board out →
red but no sentence (701 was still raised silently at runtime); fixed in 8749de3 (spoken at once,
first reminder after a minute, then every 10 min) and confirmed: board back → "NFC reader is
back", red gone. 1.3.16 (4d7618f, app sha c9dbc203…, USB sha 3560af46…) carries those two fixes
and is the one to publish and promote; 1.3.15 stays on beta behind it.

## 1.3.17: network services never started on a provisioned unit (2026-10-29 night)
TT26-00001 (the app developer's unit, revived and re-flashed with 1.3.16 at the station): WiFi
provisioned and connected, but "NET: waiting for memory before starting the network services
(43659 free)" every 10 s for the whole boot: no token, no WebSocket. The gate in
`serviceNetworkStart()` wanted 55 KB free internal; a provisioned unit has the WiFi stack up, the
BLE memory released and the 32 KB internal DMA bounce for the SD writer taken at boot (demo unit
for scale: 88 KB before BLE, 47.7 KB with BLE active). TLS and the sockets are in PSRAM
(sdkconfig), so 55 KB was more than the start needs. dc0199f: the bounce is 16 KB; after 60 s on
WiFi the gate accepts 36 KB free with a 10 KB largest block (logged); 5 min below that raises
803 HEAP_LOW once. 1.3.17 (c24c4a8, app sha dfdd41af…, USB sha 2877692d…) carries it; publish,
then re-flash TT26-00001 from the station (its card also has the old 26-file pack: the new one
downloads by itself once the network is up).

## 1.3.17 promoted to stable (2026-10-30)
Promoted on Mert's word with the voice pack "Yelda V3 (fault codes)" paired; 1.3.16 (the build
with the network-start gate bug) moved back to beta so nothing lands on it; 1.3.15 stays on beta.
Stable: 1.3.17 > 1.3.14. Station trap closed on the portal (edf12f9): a flash erases the unit's
identity, so the enrollment step can no longer be skipped for a unit without keys, and the
"known unit, erased" restore keeps the pairing code (label stays valid). TT26-00001 is being
re-flashed and restored at the station.

## Pairing, card swap and download speed on the bench (2026-10-30)
Demo unit (TT26-00002, 1.3.17 + bench): card pulled → 307 red → a *different* (faster) card put
in → remounted, "card pack none, 34 required files missing" → the pairing from the app went
through (credentials, portal probe, token, WebSocket) but spoke nothing: the new card had no
/sounds at all, not a firmware fault. The unit then staged the full pack from the portal (34
files, ~1 min) and activated it by itself, as designed. Pairing-time heap: 37.6 KB free with BLE
and WiFi both up, min 6.8 KB seen with BLE up during `ft begin` (thin; the station's
intermittent BLE-test failure still needs a failing session log). Speeds on the new card: writes
7.6 MB/s (`sdwritebench`); a 4 MB track 1.52 Mbps with WiFi modem sleep on, 2.26 Mbps with it off
(now off while downloads run, a981…: see git), SD writes 0.7 s of the transfer. The WiFi buffer
settings were tried smaller and reverted (no gain, and download speed is a priority).

# TilkieTalkie firmware — status and remaining work

Branch `IDF-Migration` is the production branch (ESP32-S3-WROOM-1-N16R8, Arduino 3.3.12 on
ESP-IDF 5.5.5 via the pioarduino platform, env `esp32-s3-devkitc-1`). Full findings: `AUDIT-2026-10-11.md`.
Bench device: `/dev/cu.usbmodem21201`; flash backup of the previous device build in `backups/` (git-ignored).

## Status (2026-10-12, after the unattended night — see NIGHT-REPORT-2026-10-11.md for the narrative)

Done, bench-verified, committed (f33317e..b051d28):

| Area | What changed | Measured |
|---|---|---|
| Build | pioarduino stable, regenerated sdkconfig/lock; builds in ~90 s | — |
| Diagnostics | `Diag` module: per-section loop timing, `timing`/`tasks`/`coredump`, reset reason, core dump to flash | — |
| Memory | mbedTLS + BT to PSRAM, fewer static WiFi buffers, PSRAM memtest off, bootloader INFO | idle internal heap 42 → 100 KB; min free 1.6 → 30 KB (during a download) |
| Battery | config re-applied only on device (re)appearance; telemetry/diagnostic polls split; I2C bus recovery; bus-free wait | `battery.update()` worst 603 → 185 ms; loop gap 750 → ~205 ms; loopTask CPU 33 → 19 % |
| Charger safety | CE asserted only after 3.6 V read-back; ITERM 120 mA; VRECHG 250 mV; fast tier 1 A; no kick after Charge Done | — |
| Gauge | real Chem ID via MAC 0x0006 (**0x1210 = Li-ion default**); LFP charge profile (0x4666/466E/4676/467E = 3600, ranges 2900/3300/3450, term voltage 100 mV, taper 250, design voltage 3200, BCAST off, pacing repaired) idempotent at boot; DF write verify 400 ms | gauge moved from PV to HV region |
| Downloads | owner-side completion (no more callback UAF/semaphore crash); 4 MB PSRAM ring + 32 KB bounce; status-line check; sha256 while writing; atomic JSON; backoff; terminal-only failure callbacks; no Google ping; free-space cache | 10 MB: crash → 36–39 s; no loop stall > 220 ms |
| Figure flow | cache-first; existence-only check; play when first track exists; updates merged; skip missing tracks; 401 refresh; no body logging | sound ~260 ms after tap |
| Bench | `tap <uid>`/`untap` simulation, `dfread/dfwrite8`, `sdwritebench`, `cat`, tools/ scripts | — |

Bench facts to remember: the SD card (macOS-formatted, 30 GB) pauses ~3.0 s every 55×32 KB writes
and sustains only ~0.3 MB/s; the portal currently returns an empty manifest for unit
`04-1F-76-D2-56-68-80`; a power bank switches off whenever firmware isn't driving charger CE.

## Remaining work (in order)

### A. Audio (AudioController) — done 2026-10-11, needs a listening check by the owner
Rewritten (`src/AudioController.*`, `src/ReadAheadFileSource.*`): ES8388 brought up in the ADF order (VMID on, DAC-only,
DACPOWER 0x2C = LOUT1 speaker + LOUT2/ROUT2 jack, jack 6 dB under the speaker, starts muted); AMP_CTRL (GPIO41) driven
open-drain (LOW = speaker off, released = board/jack decide; the level while released reports a plug); I2S driver installed
once at boot and kept (MCLK never drops, APLL off, 16 DMA buffers, mono L+R mix for the single speaker); libmad workspace
allocated once; audio task priority 12 on core 1; volume = codec attenuation only (software gain 1.0), NVS write deferred
2 s; pause/resume keep the decoder (resume continues exactly where it stopped), beep no longer advances the playlist;
`end()` mutes, powers the codec down, drops the amp and releases I2S before deep sleep; 128 KB PSRAM read-ahead with its
own reader task (core 0, prio 6) so the decoder never waits on the card. Console: `volume <n>`, `audio`, `amp on|off|auto`.
Verified on the bench: play/pause/resume/next/prev, 15 track changes without heap drift, a 1 MB download during playback
with 2–3 s card stalls and zero underruns, seek-in-track pending (Reverb path).
Open: (1) the owner has to confirm the speaker still sounds right at low volume and that a plugged headphone mutes it
(`audio` shows "headphone plugged"); if GPIO41 turns out not to be NS4150B CTRL, `amp` handling is a two-line revert;
(2) no `/sounds/beep.mp3` on the bench card, beep path untested; (3) seek assumes 96 kbps CBR (12 kB/s).

### B. Network (ReverbClient / WiFiProvisioning / RequestManager) — mostly done 2026-10-11
Done: TLS with the embedded CA bundle for the API, wss and /broadcasting/auth (the old setBaseUrl() downgraded https to
http); channel auth runs from update() with 5/10/20/40 s backoff and a token refresh on 401/403; subscription confirmed on
`pusher_internal:subscription_succeeded`; reconnects back off 2 s → 60 s; device report on change / 5 s position while
playing / 60 s heartbeat (was 1/s), SSID dropped; commands for another device_id ignored; remote `play <path>` limited to
/assets/; report buffer 768 B with an overflow guard; USB console `setTxTimeoutMs(5)` so an unread port cannot stall the loop.
Remaining: subscribe to `private-device.{uuid}` once the backend flag `TILKIE_PUBLIC_DEVICE_CHANNEL` is turned off (backend
dual-publishes already); per-unit provisioning PoP (factory topic); esp_websocket_client on its own task (nice to have);
BT controller memory release when booting already provisioned (needs reboot-into-provisioning flow).

### C. Power task / runtime — day 2 evening: manifest fetch / OTA check / status posts moved to a request worker task (loop never waits for HTTP); chargemon non-blocking; flash auto-suspend evaluated and left off
- Done 2026-10-12: battery poll on its own task (bus lock), LED on its own task with scenes, quiet diagnostics,
  USB console non-blocking, BLE controller memory released on provisioned boots, PSRAM threshold 2 KB, tap debounce.
  Still open: RequestManager HTTP on the loop task (an 8 s manifest timeout can still freeze the loop), temp-guard
  sanity range, NFC finite retries + RF off, pogo ext1 wake loop, monitor-phase sleep, SD unmount before rail-off,
  console/log gating for release builds, play-while-downloading for faster time-to-first-story.
- Internal heap (2026-10-11, after `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 16 KB→4 KB, AsyncTCP stack 16→8 KB, audio task
  10→8 KB): idle 50 KB (largest block 31 KB), playing 43 KB, playing + downloading 21 KB, min since boot 11.7 KB (was 716 B
  with largest block 3 KB before the tuning). Still thin: Reverb's TLS WebSocket costs ~60 KB internal at connect; BT
  controller memory is only released after a provisioning run (`FREE_BTDM`), never when booting already provisioned —
  releasing it at boot means re-provisioning must reboot into provisioning mode.
- Loop latency still measured on the bench (2026-10-11, after moving stale erases to the SD task): manifest fetch+parse
  `request` 241 ms, download finalize/manifest save `files` up to 667 ms, worst loop gap 1.4 s during a 5-track download.
  Candidates: parse manifests off the loop (request task), make writeJsonAtomic/pending-manifest handling incremental.
1. Move battery polling to its own task (core 0, prio ~6) so the remaining ~55 ms/s + 185 ms/10 s leave the loop; temperature guard sanity range (a bad read suspended charging at "4629 °C").
2. Async boot: buttons/NFC/audio/LED first; no 5 s + 10 s waits; `wifiProv.begin` 30 s wait off the loop.
3. NFC: finite passive-activation retries + RF off after a read (RF field left on all session); init retry.
4. Pogo pin in ext1 wake mask: a figure left on → sleep/wake/autoplay loop; monitor phase never sleeps; unmount SD before rail-off.
5. LED single owner with priorities; no NVS writes per LED pass.
6. Console: table-driven, dangerous commands (`sdformat`, `pdvolt 20`, `gaugeprog`, `learndis`, `poweroff`, `factory`) behind a factory/dev flag; no unbounded `while(!Serial.available())`.
7. Release hygiene: `-O2`, log levels, TWDT panic once blocking calls are gone, `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`.

### D. Battery — 2026-10-12: charger input budget, gauge FET options, full-charge hold and coulomb-counted SOC done (see NIGHT-REPORT); still open: bqStudio LiFePO4 chemistry + learning cycle (gauge SOC/FCC are garbage until then), hardware: ~0.3 Ω pack path and VSYS dependence on the battery path
1. EV2400 + bqStudio: run GPCCHEM (the ESP can record the discharge log), program the LFP Chem ID, calibrate CC gain against a reference current, run the learning cycle (procedure in the audit §3.6), export the golden image; firmware replays the `.df.fs` per unit at the factory.
2. Fallback if no EV2400: firmware-side SoC from coulomb counting + LFP rest-voltage table.
3. Fix `learndis`/`learnrest` (floor above Term Voltage, charging re-enabled at floor, rest under load).
4. Hardware: path resistance (~100 mΩ), cell connector rating, NTC in production packs (Temperature Enable 0x02 then).

### E. Backend — done 2026-10-12 except the device-scoped token
Private `private-device.{uuid}` channel only (Firebase, Sanctum and web guards on /broadcasting/auth), commands only via
`POST /api/hubs/{hub}/command` (allowlist in config/tilkie.php) and the admin route, device reports as `client-device-report`
validated by the listener (subscribed sender, matching device_id), hub secrets (`hub:secret`, POST token with X-Hub-Secret),
rate limits, XSS and Horizon fixes, remember-token fix, 148 tests green. `docs/MOBILE-APP-MIGRATION.md` for the app developer,
`docs/SECURITY-CHANGES.md` for the state (incl. the Reverb client-event relay caveat). CI: `.github/workflows/deploy.yml` +
`deploy/deploy.sh` (needs the four GitHub secrets, see deploy/README.md). Open: device-scoped token (with the factory flow),
MySQL, APP_DEBUG=false before launch, a Reverb check in front of client events.

#### E (old notes)
1. Replace `GET /api/hubs/{uuid}/token` with a factory-provisioned per-device secret → device-scoped Sanctum token on the Hub model.
2. Private `private-hub.{id}` channels, server-side command publishing, remove the `X-Client-Source` bypass, Reverb `allowed_origins`.
3. Fix the `appScheme` XSS; `APP_DEBUG=false`; Horizon gate; rate limits; ownership checks on `/api/units/{nfc_id}` and content binding.
4. Device API v2: compact manifest with ETag/304, signed media URLs, server-side transcode, status via HTTPS.
5. MySQL/Postgres + string FKs before real traffic.

### F. OTA + factory mode — OTA done 2026-10-12 (device + backend); factory mode planned in docs/FACTORY-MODE.md (evening of day 2), implementation next
Device: `src/OtaManager.*` (download task on core 0, Arduino Update into the spare partition, SHA-256 over the stream, Range
resume on dropped connections, status posts from the loop task, rollback-safe: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, the
new image confirms itself when Reverb connects or after 30 s, otherwise the bootloader returns to the previous image and the
device reports `rolled_back`). Version = `CONFIG_APP_PROJECT_VER` (sdkconfig) → app descriptor → parsed by the backend upload.
Updates start only when idle (no playback/figure/download, charging or ≥ 40 %); mandatory ones force after 1 h; periodic
check 24 h; remote `ota-update <release_id>`. Backend: branch `feature/ota` (merged on the bench): firmware_releases,
hub firmware columns, hub_firmware_events, selector (pin > channel/rollout/min_version), signed 15-min download URLs with
Range, admin Firmware pages + Hub firmware card, 28 tests, docs/OTA.md.
Verified on the bench: 1.1.0 → 1.1.1 via the backend (15 s download+flash), success reported; crashing 1.1.3 rolled back by the
bootloader and reported. Open: factory mode (serial-number enrollment, `hub:secret` is the backend half), flash auto-suspend so
LED/WebSocket keep running during the flash writes (`CONFIG_SPI_FLASH_AUTO_SUSPEND`), production nginx (PHP's dev server was
flaky on long downloads).

## Local portal for bench testing (done 2026-10-11)
The backend repo (`/Users/mert/Herd/portal.tilkietalkie.com`, branch `develop`) now has `scripts/bench/` — see its README.
`scripts/bench/bench.sh up` starts `artisan serve` (:8000, 4 workers), `reverb:start` (:8080, plain ws, app key `ttbench`) and a
redis queue worker on the Mac's LAN IP with every override injected as process env (own `database/bench.sqlite`, own redis
prefix; Herd and `.env` untouched). `bench.sh seed` generates spoken test MP3s (`gen_tracks.py`, macOS `say` + lameenc) and runs
`php artisan bench:seed`: user `bench@tilkietalkie.local`/`bench1234` (admin), hub `250696234569532` (this board), units
`04-1F-76-D2-56-68-80` (Bench Fox, 5 tracks / 2.9 MB), `04-BE-11-C2-00-00-01` (Bench Owl, 1 track), `04-BE-11-C2-00-00-02`
(Bench Wildcard: 2 custom tracks + unit-scoped pack). `bench.sh check` curls token + manifest; `bench.sh device` prints:
`endpoint set http://<ip>:8000/api <ip> 8080 0 ttbench` + `restart` (NVS-backed; `endpoint clear` + `restart` returns to production).

Verified on the device: token refresh after the stale production token is rejected, Reverb subscribe + device-report relay
(listener → queue → `device.status.updated`), tap → manifest → 5 downloads at ~2.4 Mbps with sha256 verified and play-as-they-arrive
merges, re-tap cache hit with `manifest unchanged`, wildcard custom tracks, stale-asset removal. Re-seeding changes all ids, which
reproduces the "same UID, different figure" case (cached content plays, new manifest arrives) — used to fix the merge-vs-restart bug.

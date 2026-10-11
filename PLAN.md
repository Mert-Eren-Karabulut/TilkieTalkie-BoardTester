# TilkieTalkie firmware — status and remaining work

Branch `IDF-Migration` is the production branch (ESP32-S3-WROOM-1-N16R8, Arduino 3.3.12 on
ESP-IDF 5.5.5 via the pioarduino platform, env `esp32-s3-devkitc-1`). Full findings: `AUDIT-2026-10-11.md`.
Bench device: `/dev/cu.usbmodem21201`; flash backup of the previous device build in `backups/` (git-ignored).

## Status (2026-10-11)

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

### A. Audio (AudioController) — next
1. ES8388 init: enable VMID (0x00 = 0x12 per ADF sequence), DACPOWER 0x2C so LOUT2/ROUT2 (headphone jack) are powered, set 0x30/0x31 volumes with a lower headphone ceiling; mute-before-config, unmute with ramp.
2. Drive the amp enable (GPIO41 = AMP_CTRL, currently INPUT): HIGH while playing, LOW idle/sleep; read it as INPUT_PULLUP briefly to detect a plug (confirm jack wiring with owner).
3. Install the I2S driver once (subclass `AudioOutputI2S` so `stop()` doesn't uninstall), `APLL_DISABLE`, `dma_buf_count` 16; stop the 23 ms tail truncation and pops.
4. Fix the 8 KB PSRAM leak per track start (buffer allocated in `play()`/resume/seek/beep, never freed): allocate once in `begin()`.
5. Audio task priority ~12–16 on core 1; `SDWriteTask` already on core 0.
6. Volume: write NVS 2 s after the last change (currently every step, inside the audio mutex); software gain fixed at 1.0, codec-only attenuation.
7. Pause/resume without destroying the decoder; beep must not advance the playlist; codec mute + amp off before sleep.
8. SD read-ahead task with a 64–128 KB PSRAM buffer so the decoder tolerates the card's 3 s pauses (dropouts during downloads).

### B. Network (ReverbClient / WiFiProvisioning / RequestManager)
1. Remove `reboot`/`restart`, `set-max-volume`, `play <path>` from remote commands; require `device_id == own`; rate-limit.
2. Status report: on change + 60 s heartbeat; drop SSID and NFC UID; fix the 512 B frame overflow (silent drops).
3. Reconnect with exponential backoff; subscribe without the HTTPS auth POST inside the WS callback (public channel today); start services on GOT_IP (device stays offline forever if WiFi is late at boot); make `begin()` idempotent (the `reverbstart` leak).
4. HTTPS for the API (CA bundle is compiled in; `MBEDTLS_EXTERNAL_MEM_ALLOC` now on) — owner agreed: HTTPS for API, HTTP for media with sha256.
5. Deinit provisioning at PROV_END (BLE memory); per-unit PoP instead of `abcd1234`.
6. ~~Configurable API/Reverb host for local testing~~ done (`endpoint` command).
7. Later: `esp_websocket_client` in a dedicated net task; device-scoped token (backend work).

### C. Power task / runtime
1. Move battery polling to its own task (core 0, prio ~6) so the remaining ~55 ms/s + 185 ms/10 s leave the loop; temperature guard sanity range (a bad read suspended charging at "4629 °C").
2. Async boot: buttons/NFC/audio/LED first; no 5 s + 10 s waits; `wifiProv.begin` 30 s wait off the loop.
3. NFC: finite passive-activation retries + RF off after a read (RF field left on all session); init retry.
4. Pogo pin in ext1 wake mask: a figure left on → sleep/wake/autoplay loop; monitor phase never sleeps; unmount SD before rail-off.
5. LED single owner with priorities; no NVS writes per LED pass.
6. Console: table-driven, dangerous commands (`sdformat`, `pdvolt 20`, `gaugeprog`, `learndis`, `poweroff`, `factory`) behind a factory/dev flag; no unbounded `while(!Serial.available())`.
7. Release hygiene: `-O2`, log levels, TWDT panic once blocking calls are gone, `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`.

### D. Battery (needs owner/hardware input)
1. EV2400 + bqStudio: run GPCCHEM (the ESP can record the discharge log), program the LFP Chem ID, calibrate CC gain against a reference current, run the learning cycle (procedure in the audit §3.6), export the golden image; firmware replays the `.df.fs` per unit at the factory.
2. Fallback if no EV2400: firmware-side SoC from coulomb counting + LFP rest-voltage table.
3. Fix `learndis`/`learnrest` (floor above Term Voltage, charging re-enabled at floor, rest under load).
4. Hardware: path resistance (~100 mΩ), cell connector rating, NTC in production packs (Temperature Enable 0x02 then).

### E. Backend (security prerequisites before OTA)
1. Replace `GET /api/hubs/{uuid}/token` with a factory-provisioned per-device secret → device-scoped Sanctum token on the Hub model.
2. Private `private-hub.{id}` channels, server-side command publishing, remove the `X-Client-Source` bypass, Reverb `allowed_origins`.
3. Fix the `appScheme` XSS; `APP_DEBUG=false`; Horizon gate; rate limits; ownership checks on `/api/units/{nfc_id}` and content binding.
4. Device API v2: compact manifest with ETag/304, signed media URLs, server-side transcode, status via HTTPS.
5. MySQL/Postgres + string FKs before real traffic.

### F. OTA + factory mode
Firmware: version reporting, check/download/verify, `esp_ota_*` with rollback enabled. Backend: firmware_releases table,
rollout %, admin upload page, device check endpoint (design in the audit §4). Factory: enrollment with serial numbers
(owner: replaces eFuse MAC as identity), QA upload, factory build flag.

## Local portal for bench testing (proposal)
Run the Laravel app on the Mac bound to the LAN (`php artisan serve --host 0.0.0.0 --port 8000`, `php artisan reverb:start
--host 0.0.0.0 --port 8080`, `QUEUE_CONNECTION=sync`), seed a bench dataset (hub uuid = device eFuse MAC decimal, a bound user,
unit `04-1F-76-D2-56-68-80` with a figure/content/episodes/tracks and MP3s in `storage/app/public`), and point the device at the Mac with the firmware's
`endpoint set http://<mac-ip>:8000/api <mac-ip> 8080 0 <local-reverb-key>` (NVS-backed, done; `endpoint clear` reverts to production). Media URLs follow `APP_URL`/request host, so they resolve to the Mac automatically.

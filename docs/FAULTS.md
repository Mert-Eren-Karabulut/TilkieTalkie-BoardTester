# Fault codes: one system for the toy, the portal and support

Status: design (2026-10-16), implemented from firmware 1.2.18 and the matching portal release.
Replaces the ad-hoc `last_error` fruit string with a catalogue every side shares.

## Goals

- **One catalogue, one source.** Every fault is declared once, in the firmware
  (`src/Faults.h`, an X-macro list): numeric code, slug, severity, the child-facing fruit
  word (if the toy says it), a one-line description and a remedy for support. The image
  embeds the catalogue (`TTFAULTS1:` marker, like the prompt schema) so the portal learns
  each release's codes when the `.bin` is uploaded. Nothing is typed twice.
- **Three audiences, three vocabularies.** The child hears a fruit (Elma, Armut, …); the
  parent reads the fruit in the app; support and engineering use the number and slug. The
  admin panel shows all three together with the description and remedy.
- **Fleet scale.** Faults are events with counts, not strings in a column. Thousands of
  toys × a handful of events a day stays queryable: an indexed `device_events` table,
  aggregation per code / firmware / model, 90-day retention.
- **Nothing is lost on reboot.** A panic, a watchdog reset, a brownout are reported by the
  next boot with the reset reason and the previous uptime.

## Codes

Ranges by subsystem; severity `info` (telemetry only), `warning` (visible in the admin,
silent on the toy), `error` (prompt if a fruit is mapped, badge in the admin), `critical`
(red LED scene, prompt, alert in the admin).

| code | slug | sev | fruit | when |
|---|---|---|---|---|
| 101 | WIFI_NOT_PROVISIONED | info | | boot without credentials |
| 102 | WIFI_AUTH_FAILED | error | üzüm | credentials rejected during pairing |
| 103 | WIFI_NO_LINK | error | portakal | home network in range, no link for 60 s |
| 104 | WIFI_OUT_OF_RANGE | info | | provisioned, network not in range (toy away from home) |
| 105 | WIFI_RADIO_RESET | warning | | reconnect escalation reset the radio |
| 106 | PORTAL_UNREACHABLE | warning | | API request failed at transport level |
| 107 | TOKEN_REJECTED | error | | 401 even after a token refresh |
| 108 | WS_AUTH_FAILED | error | | channel authorisation refused |
| 109 | WS_DISCONNECTED | info | | socket dropped (counted) |
| 110 | NOT_LINKED | info | kivi | online, but `GET /api/hubs/{uuid}` says `bound: false` (or the legacy token endpoint answered 404); raised at boot and then daily, cleared (`Faults::clear`) once bound; the `not_linked` prompt is spoken once per boot after the hello, idle only, at most daily |
| 112 | NOT_ENROLLED | info | | no factory pop at boot (detail `no pop`), or the token POST answered 404 (detail `404`) / 401 (detail `secret`); retried every 15 min |
| 113 | CAPTIVE_PORTAL | warning | nar | the portal probe (`/storage/ping.json`) failed: during pairing the network is rejected and `setup_captive` is spoken; a provisioned toy stays offline silently and re-probes every 2 min |
| 201 | FIGURE_UNKNOWN | error | elma | portal has no unit for this card |
| 202 | FIGURE_NO_CONTENT | error | elma | figure exists, nothing to play |
| 203 | FIGURE_OFFLINE | error | armut | portal unreachable and nothing cached for the card |
| 204 | DOWNLOAD_FAILED | error | kiraz | a track gave up after its retries |
| 205 | TRACK_MISSING | error | | a cached track vanished before playback |
| 206 | MANIFEST_INVALID | error | | manifest without the fields we need |
| 207 | DOWNLOAD_STALLED | warning | | transfer stalled (resumed or retried) |
| 208 | CONTENT_SYNC_FAILED | warning | | background re-check failed |
| 301 | SD_MOUNT_FAILED | critical | muz | card missing or unreadable at boot |
| 302 | SD_WRITE_FAILED | error | muz | write or rename failed |
| 303 | SD_FULL | error | muz | no space for a download |
| 304 | SD_FILE_CORRUPT | warning | | manifest / queue / pack json unreadable (recreated) |
| 305 | SD_SLOW | info | | write stall above 3 s |
| 306 | CARD_FOREIGN | info | | `/.device` on the card names another toy: its story cache was wiped (detail `prev=<uuid>`) |
| 401 | AUDIO_INIT_FAILED | critical | | codec / I2S init failed |
| 402 | DECODER_FAILED | error | | a file could not start decoding |
| 403 | AUDIO_UNDERRUN | info | | read-ahead ran dry (counted) |
| 404 | PROMPT_FILE_MISSING | warning | | pack incomplete on the card |
| 405 | PLAYLIST_STUCK | warning | | waited for a download for too long |
| 501 | BATTERY_LOW | info | | 20 % and below on battery |
| 502 | BATTERY_CRITICAL | warning | | cutoff reached, going to sleep |
| 503 | GAUGE_NO_TELEMETRY | error | | gauge not answering |
| 504 | CHARGER_FAULT | error | | charger fault bits set |
| 505 | BROWNOUT_RESET | error | | reset reason brownout |
| 506 | DEAD_CELL_RECOVERY | warning | | recovery hold engaged |
| 507 | POWER_BUS_ERROR | warning | | I2C error on the power bus |
| 601 | OTA_CHECK_FAILED | warning | | firmware check failed |
| 602 | OTA_DOWNLOAD_FAILED | error | karpuz | image download failed |
| 603 | OTA_FLASH_FAILED | error | karpuz | flash write / activate failed |
| 604 | OTA_SHA_MISMATCH | error | karpuz | image hash did not match |
| 605 | OTA_ROLLED_BACK | error | | bootloader returned to the previous image |
| 606 | OTA_CRASH_LOOP_REVERT | critical | | crash-loop guard switched partitions |
| 607 | PROMPT_PACK_FAILED | warning | | pack could not be downloaded |
| 608 | PROMPT_PACK_ACTIVATE_FAILED | error | | staging could not be swapped in |
| 701 | NFC_INIT_FAILED | error | | reader not found at boot |
| 702 | NFC_READ_TROUBLE | warning | | figure on the pins, no card read |
| 703 | NFC_RESET | info | | reader watchdog reset |
| 801 | WATCHDOG_RESET | error | | task watchdog reset |
| 802 | PANIC_RESET | error | | panic / exception reset |
| 803 | HEAP_LOW | warning | | internal heap below threshold |
| 804 | LOOP_STALL | warning | | a loop section took longer than 2 s |
| 805 | STACK_LOW | warning | | loop task stack under 1 KB |
| 806 | CONFIG_CORRUPT | error | | NVS record unreadable, defaults restored; also: the NVS partition was full and reformatted at boot (detail `nvs full, reformatted [-<namespace>...]` names what could not be restored), or the requestmgr namespace was full and erased alone |

The fruit list stays short on purpose (a child can repeat one word): elma, armut, kiraz,
üzüm, portakal, karpuz, muz, kivi, nar.

### NVS without losing the factory identity

`ESP_ERR_NVS_NO_FREE_PAGES` at boot used to erase the whole partition (two code paths, plus the
Arduino core's own fallback in `initArduino()`), which would also destroy the factory secret
and pop. Now `ConfigManager` (it runs first, at static construction) mounts the partition
read-only, copies every entry to PSRAM, reformats, and writes the entries back, the `factory`
namespace first, then `config`, `nvs.net80211` (WiFi), `settings`, `faults`, `requestmgr`, the
rest. A namespace that no longer fits is erased as a whole and named in the 806 detail. A
format from a newer IDF (`NEW_VERSION_FOUND`) cannot be read and is erased (806
`nvs newer format, erased`). At run time a namespace that cannot be written (the uid
mappings) is erased alone, never the partition.

### WiFi: a stored password that stopped working

A docked toy whose router password changed used to restart every ~15 min and say hello +
"I cannot connect" each time, all night. After 3 consecutive auth-type disconnects (auth
fail, 4-way/handshake timeout, MIC failure) on a provisioned toy: no radio resets, no idle
restarts, one silent reconnect attempt every 30 min, `wifi_lost` (103 with the hint) at most
every 6 hours, and the amber offline LED only for 30 s around each attempt. A successful
connection or a "network not found" ends the streak.

## Firmware

`Faults` (src/Faults.h/.cpp):

- `Faults::raise(Fault code, const char *detail = nullptr)` – records the event (uptime,
  boot sequence, short detail such as `uid=04-1F-…` or `http=404`), updates the active set
  for persistent faults, speaks the mapped prompt through `VoicePrompts` (its policy still
  decides when), sets the error LED scene for `critical`, and queues the event for the
  portal. The same code is reported at most once per 10 minutes; repeats are counted.
- `Faults::clear(Fault code)` – ends an active fault (the clearing is reported too). Each
  code declares how it clears: `manual` (next raise/clear call), `on_network` (WiFi back),
  `on_play` (a figure plays), `on_boot`.
- `Faults::active()` – the active set for the device report; `Faults::fruit()` – the fruit
  of the most severe active (or latest) fault, kept in `last_error` for older portal code.
- Persistence: a 32-entry ring in NVS (`faults` blob) plus RTC-noinit copies of the last
  code and uptime. At boot the reset reason is classified (`801`, `802`, `505`, or none) and
  reported with the previous uptime, then the unsent entries are uploaded.
- Upload: `POST /api/hubs/{uuid}/events` with up to 20 events
  `{code, slug, severity, detail, count, uptime_s, boot, firmware, age_s}`; `age_s` lets the
  portal compute the real time without a clock on the toy. Sent 5 s after a raise (debounced)
  and at boot once the token is there; entries stay in NVS until the portal answers 200.
- Device report: `"faults":{"active":[{"c":103,"since_s":120}],"last":{"c":204,"age_s":30,"d":"…"}}`
  next to the existing `last_error`.

Where today's code sets `ReverbClient::setLastError("elma")` it calls
`Faults::raise(Fault::FIGURE_NO_CONTENT, uid)`; where it says a prompt for a failure, the
prompt is spoken by `Faults` instead (one place decides).

## Portal

- `device_events` (hub_id, code, slug, severity, detail, count, firmware_version, uptime_s,
  occurred_at, created_at; indexes on (hub_id, occurred_at), (code, occurred_at),
  (severity, occurred_at)); 90-day pruning.
- `hubs.active_faults` (json), `hubs.last_fault_code`, `hubs.last_fault_at` from reports and
  events; `firmware_releases.fault_catalog` (json) read from the `TTFAULTS1:` marker; a
  committed copy of the current catalogue (`resources/device-faults.json`, exported by
  `tools/faults.py`) for hubs whose release is unknown.
- `POST /api/hubs/{hub}/events` (hub token): validates, stores, updates the hub, broadcasts
  `hub.events.updated` for the open hub page.
- Admin: hub page "Events" timeline (faults and firmware events merged, newest first, with
  description and remedy); Fleet → **Faults** page: counts per code for 24 h / 7 d, affected
  hubs, by firmware and model, drill-down to the hubs; hubs list and dashboard show hubs
  with active `error`/`critical` faults; the hub header shows the fruit + code so a support
  call ("it said Elma") resolves in one search.

## Why not just strings

A string per hub tells you the last thing that went wrong on one toy. Codes with counts and
timestamps tell you that `204 DOWNLOAD_FAILED` rose 10× on firmware 1.2.17 in Germany this
week, which toys, and whether it stopped after the fix. That is what large-scale operation
needs.

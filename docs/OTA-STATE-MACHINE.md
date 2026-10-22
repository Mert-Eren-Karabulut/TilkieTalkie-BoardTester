# OTA state machine (dock only)

Firmware side of docs/PROTOCOL-V2.md §5. Code: `src/OtaManager.cpp` (comments cite the rows as
"table: ..."), the conditions come from `otaConditions()` in `src/main.cpp`, sleep gating in
`src/SleepController.cpp`. Every state × event below has one defined outcome; an event not
listed for a state is ignored in that state.

## States and the data they keep

| state | meaning | kept |
|---|---|---|
| IDLE | nothing offered | next check time, last answer time |
| CHECKING | `GET /hubs/{uuid}/firmware/check` in flight (60 s timeout) | purpose: periodic / command / re-offer / url-expired |
| WAITING | a release is offered, the toy waits for the dock and an idle moment | the offer **in RAM** with its time, first-offer time (mandatory hour), "announced" |
| PROMPTS | the voice prompt pack paired with the release is staged into `/sounds.staging` | the offer |
| DOWNLOADING | the image streams into the inactive partition (task, Range resume, sha256 on the fly) | the offer, "re-check used" |
| APPLYING | sha256 matched, `Update.end()` activates the partition | |
| REBOOTING | the final status is posted (≤ 10 s), then `ESP.restart()` | NVS `ota_rel` / `ota_vcode` |
| FAILED | the attempt failed; 10 min hold, then IDLE | failure kind + text |

NVS (survives reboots): `ota_rel`, `ota_vcode` (what the next boot should be), `ota_revert`
(crash-loop guard fired), `ota_last_rel` (last release applied), `ota_rb` (rolled-back release
ids, the last 4). The offer itself is never persisted: after any reboot or sleep the boot check
re-offers.

## Conditions

- **gate** (leave WAITING): `docked` (VBUS) AND not `playing` (story or prompt) AND not `paused`
  AND no `figure` on the pins (pogo switch or a read card) AND no story download in flight.
- **forced gate** (mandatory release, first offered ≥ 1 h ago): `docked` only.
- **low battery**: on battery (no VBUS) AND gauge telemetry valid AND SOC < 15 %. (On the dock
  the cell is charging; nothing aborts.)
- **stale offer**: offer received more than 10 min ago.
- **offer acceptance**: `explicit` (portal: pin or `ota-update` command; absent = false) OR
  (`version_code` > running AND release id not in `ota_rb`). Otherwise status `declined`.

## Table

`→` = next state. "post X" = `POST /hubs/{uuid}/firmware/status {status: X, release_id, error,
failure?, version_code: running}`.

### Boot, checks, answers

| state | event | → | side effects |
|---|---|---|---|
| (any) | boot | IDLE | first check at 90–120 s; pending-verify image: confirmation timer starts; crash-loop guard (3 crash resets before 5 min of uptime → other partition, `ota_revert`) |
| IDLE | timer (90–120 s after boot, then hourly) | CHECKING | only with WiFi up and the network services started |
| IDLE | dock on, last answer > 10 min ago | CHECKING | (the boot check is not preempted) |
| IDLE / WAITING / FAILED | nudge (`ota-update`, value null) | CHECKING | purpose command |
| IDLE / WAITING / FAILED | explicit command (`ota-update <release_id>`) | CHECKING | `&release_id=` sent; the portal answers `explicit: true` |
| CHECKING | nudge / explicit command | CHECKING | queued, sent right after the current answer |
| PROMPTS / DOWNLOADING / APPLYING / REBOOTING | nudge / explicit command | (same) | ignored, logged |
| CHECKING | answer: nothing (`release_id: null` / 204) | IDLE | offer dropped (logged "no longer offered"); prompt pack of the running release synced if it differs; stale staging dropped |
| CHECKING (url-expired) | answer: nothing | FAILED | post failed (`release no longer offered`) |
| CHECKING | answer: release, accepted | WAITING | offer + time stored; first-offer time kept if it is the same release |
| CHECKING | answer: release in `ota_rb`, not explicit | IDLE | post **declined** (`error`: rolled back on this device before) |
| CHECKING | answer: `version_code` ≤ running, not explicit | IDLE | post **declined** (`error`: not newer than the running firmware) |
| CHECKING | answer: incomplete (no url / sha256 / size) | IDLE | logged (url-expired: FAILED) |
| CHECKING (url-expired) | answer: same release (id, sha256, size) | DOWNLOADING | fresh URL, download restarts from zero (dock no longer required, as in DOWNLOADING) |
| CHECKING (url-expired) | answer: another release, accepted | WAITING | post failed for the old release (`replaced by N`), new offer waits |
| CHECKING | answer: 429 | WAITING if an offer is held, else IDLE | next check after `Retry-After` (default 60 s); no fault; not "offline" |
| CHECKING | answer: 5xx | WAITING / IDLE | next check after 30 s, 2 min, 10 min (then 10 min); `601 OTA_CHECK_FAILED` from the third in a row |
| CHECKING | answer: 403 | WAITING / IDLE | RequestManager already refreshed the token once (≤ 1 per 10 min) and retried; next check in 1 h; 601 |
| CHECKING | answer: other 4xx | WAITING / IDLE | next check in 1 h; 601 |
| CHECKING | transport error / 60 s timeout | WAITING / IDLE | next check in 2 min; 601 (url-expired: FAILED, post failed) |

### WAITING

| state | event | → | side effects |
|---|---|---|---|
| WAITING | gate closed (undocked, playing, paused, figure, story download) | WAITING | nothing; undocking or a figure resets "announced" |
| WAITING | gate open, offer stale (> 10 min) | CHECKING (re-offer) | fresh signed URL; the answer replaces the offer (changed → new offer, revoked → IDLE). A failed re-check keeps the offer, next try after 2 min (429: after Retry-After) |
| WAITING | gate open, offer fresh, not announced | WAITING | "Kendimi güncelliyorum…" (`update_start`) requested; it counts as playing, so the gate closes while it speaks |
| WAITING | gate open, offer fresh, announced ≥ 1.5 s ago | PROMPTS if the release pairs a pack that is not on the card (and staging failed < 2 times), else DOWNLOADING | post prompts / downloading |
| WAITING | mandatory, waiting ≥ 1 h, docked | (as gate open) | figure / playing / paused / story download ignored; the announcement is attempted but not waited for |
| WAITING | battery low | WAITING | nothing (cannot start off the dock) |
| WAITING | inactivity on battery | (deep sleep) | allowed; the offer is lost, the boot check re-offers |
| WAITING | power loss / reset | IDLE (boot) | offer lost, boot check re-offers |

### PROMPTS, DOWNLOADING, APPLYING, REBOOTING

| state | event | → | side effects |
|---|---|---|---|
| PROMPTS | pack staged | DOWNLOADING | post downloading |
| PROMPTS | pack failed | FAILED | post failed, failure `prompts`; `607`; after 2 failures in a row the next attempt skips the pack |
| PROMPTS | undocked / figure / play | PROMPTS | continues |
| PROMPTS | battery low | FAILED | queued pack downloads cancelled (files kept for the next attempt); post failed, failure `battery`; `602` |
| DOWNLOADING | undocked / figure / play start / story download | DOWNLOADING | continues (the download is minutes; stories may play, prompts are held) |
| DOWNLOADING | battery low | FAILED | the task stops at the next block; `Update.abort()`; post failed, failure `battery`; `602` |
| DOWNLOADING | connection drop / 5xx / stall (20 s) | DOWNLOADING | up to 6 attempts, each resuming with `Range: bytes=<received>-` (206) |
| DOWNLOADING | retries exhausted / 404 / size mismatch / Range ignored | FAILED | post failed, failure `download`; `602` |
| DOWNLOADING | 403 or 410, first time in this attempt | CHECKING (url-expired) | `Update.abort()`; one re-check (see above) instead of failing |
| DOWNLOADING | 403 or 410 after the re-check | FAILED | post failed, failure `download`; `602` |
| DOWNLOADING | flash write error / `Update.begin` refused | FAILED | post failed, failure `flash`; `603` |
| DOWNLOADING | sha256 mismatch | APPLYING is skipped → FAILED | `Update.abort()`; post failed, failure `sha`; `604` |
| DOWNLOADING | nudge / command / dock | DOWNLOADING | ignored |
| DOWNLOADING | inactivity on battery | DOWNLOADING | deep sleep refused while an update is in progress |
| DOWNLOADING / PROMPTS | power loss / reset | IDLE (boot) | partial image is discarded (the next `Update.begin` rewrites the partition); a partial pack in `/sounds.staging` is reused; the boot check re-offers |
| APPLYING | `Update.end()` fails | FAILED | post failed, failure `flash`; `603` |
| APPLYING | activated | REBOOTING | NVS `ota_rel`, `ota_vcode` written; post applying was already sent |
| APPLYING | power loss before activation | IDLE (boot, old image) | partition not activated: the old image boots with no NVS record; boot check re-offers |
| APPLYING | power loss after activation, before the NVS record | (new image) | new image boots pending-verify, confirms itself; no success post — the portal clears its `ota_status` from the report's `fw` (see below) |
| REBOOTING | status delivered or 10 s | (restart) | |
| REBOOTING | power loss | (same as the restart) | |
| FAILED | 10 min | IDLE | next check immediately |
| FAILED | nudge / explicit command | CHECKING | |
| FAILED | dock / figure / battery | FAILED | nothing |

### After the reboot

| state | event | → | side effects |
|---|---|---|---|
| new image (pending verify) | portal reached: Reverb subscribed, or any 2xx/304 from the API | IDLE | image confirmed (`esp_ota_mark_app_valid_cancel_rollback`), `/sounds.old` dropped, then post **success** (`version_code` = running), `ota_rb` loses this release, NVS record cleared, `update_done` spoken at boot |
| new image (pending verify) | 10 min of uptime without the portal | IDLE | confirmed the same way |
| new image (pending verify) | deep sleep wanted | — | refused until confirmed (a wake through the bootloader would roll it back) |
| new image (pending verify) | crash / watchdog / power loss / reset before confirmation | (bootloader rollback) | the old image boots |
| old image after rollback | boot with `ota_rel`, the new partition ABORTED/INVALID | IDLE | `605 OTA_ROLLED_BACK`; post **rolled_back** with the reset reason; the release goes into `ota_rb` only if the reset was a crash (panic, task/interrupt/RTC watchdog) — not for a power loss, a user reset or USB |
| old image after crash-loop revert | boot with `ota_revert` | IDLE | `606` was raised before the switch; post rolled_back (`ota_last_rel`), release into `ota_rb` |
| old image, new partition never activated | boot with `ota_rel`, partition state not ABORTED/INVALID | IDLE | post failed (`new image did not start`), failure `flash`, no blacklist |
| (any boot) | explicit offer of a release in `ota_rb` | WAITING | accepted (pin / command override); a success removes it from `ota_rb` |

## Portal-facing details

- `GET /hubs/{uuid}/firmware/check?...&release_id=` (only for an explicit command). The
  answer's `explicit` (between `notes` and `url`) is read as a boolean, absent = false.
- Status values posted: `prompts`, `downloading`, `applying`, `failed`, `success`,
  `rolled_back`, **`declined`** (reason in `error`). Every `failed` also carries
  `failure` (`download` | `flash` | `sha` | `prompts` | `battery`) next to the `error` text.
- The periodic device report carries `"fw": {"v": "1.2.20", "code": 10220}` and
  `"prompts": {"pack": "<12 hex>", "state": "..."}`; after a USB flash (or the power-loss case
  above) the portal compares `fw.code` with its stale `ota_status`.
- Signed URL TTL on the portal: 6 h; the firmware re-checks anyway when its offer is older than
  10 min before starting, and once on a 403/410 during the download.

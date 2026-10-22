# System audit, 2026-10-22: discrepancies and gray areas (firmware 1.2.19 ↔ portal)

Three read-only passes: the contract between the two sides, the portal on its own, the
firmware on its own plus what a production line needs. This file keeps what matters, in the
order we should take it. "Decision" = a policy only the owner can set; "Defect" = clear fix.
File references: FW = this repo, P = the portal repo.

## A. Decisions to make first

1. **Device identity, enrollment and pairing.** Today the device's only identity is the chip
   MAC (decimal), which is broadcast in the BLE name and derivable from the WiFi MAC; the token
   endpoint hands out the *owner's* account token to anyone who presents that id (P
   `GET /api/hubs/{uuid}/token`, no auth unless a secret was enrolled, which the firmware does
   not support); binding a hub needs no proof of possession (first caller wins); one "Hub
   Token" per user means two toys of one family revoke each other's token in a loop; the BLE
   pairing PIN is `abcd1234` on every unit. Proposed model: at the factory each unit gets a
   secret (enrolled on the portal with the uuid), the device fetches a *device-scoped* token
   with `POST /token` + secret, binding needs the per-unit PoP printed on the label (QR), and
   unbind/re-bind revokes the device token. This is also the backbone of the factory pipeline.
2. **Entitlement policy for cards.** The manifest endpoint ignores who asks: any hub token
   reads any card, including a wildcard card's private recordings. Proposal: default figures
   stay open (any toy plays a bought figure), wildcard cards are owner-only (the owner's hubs);
   an explicit `revoked`/410 answer tells a toy to purge a cached card.
3. **What a card keeps when it changes hands.** Unbinding a wildcard card leaves the previous
   owner's purchased content and recordings on it; the next owner inherits them. Proposal:
   unbind detaches that user's contents and custom tracks from the card and syncs devices.
4. **Reset semantics.** No customer-facing reset exists (forget-WiFi keeps the token, the
   cache, the portal binding). Proposal: customer reset = WiFi + token + card cache + portal
   unbind; service reset = everything except a protected `factory` NVS namespace.
5. **Shipping state.** Undefined. Ship mode can only be entered by the low-battery cutoff and
   is impossible with USB present unless the 10 s delay bit is used; deep sleep has no timer
   wake and its current is unmeasured. Proposal: end of line = `ft ship` (delay bit, operator
   unplugs within 10 s), cell at ~40 %, documented "plug in to wake".
6. **Content formats.** The portal accepts wav/ogg/m4a for admin tracks and app recordings;
   the toy plays MP3 only, so those download and never play. Proposal: MP3 only (the admin
   converts in the browser already; the app uploads go through a server-side transcode or are
   rejected).
7. **Mandatory updates.** The flag forces an install after one hour of waiting, on battery and
   mid-story. Keep, or restrict to docked? Proposal: mandatory still requires the dock, only the
   "no figure / not playing" conditions are dropped.
8. **Open WiFi networks.** The firmware refuses to store an empty password, so a toy on an
   open or captive network never reconnects after a drop until a reboot. Support them or not?
9. **Accounts linked by e-mail.** Login falls back to e-mail without `email_verified` and
   re-keys the user's primary key (breaks the foreign keys). Proposal: a separate
   `firebase_uid` column, never change the id, link by e-mail only when verified.
10. **Operations.** Production is SQLite with `APP_DEBUG=true` and debug logging (owner
    decision until launch), no scheduler cron (pruning never runs), no backup step in the
    deploy, device commands depend on Horizon (a stopped worker delays them without expiry).
    Decide the launch cut-off for: MySQL move (blocked by the id type mismatch), backups,
    `APP_DEBUG=false`, cron, command TTL.

## B. Defects (clear fixes)

### Both sides
- **Rollback loop.** A release that rolled back is offered again every hour (pin kept, or
  automatic because its code is still higher) and the firmware keeps no blacklist. Fix: P skips
  a release whose last event for that hub is `rolled_back`; FW remembers the last rolled-back
  release in NVS. [P FirmwareReleaseSelector, FW OtaManager]
- **Success judged by the firmware's version_code formula.** P lets an admin type any code; if
  it differs from `major*10000+minor*100+patch`, a good update is reported as rolled back.
  Fix: P derives the code from the image only. Also align the formulas (FW accepts `1.2`,
  `1.2.100`; P requires three parts ≤ 99).
- **Signed firmware URL (15 min) vs waiting for the dock.** The toy may hold an offer for
  hours; the download then fails with 403 and re-checks 10 min later. Fix: FW re-checks before
  starting a job older than 10 min and on a download 403; P TTL to several hours.
- **Non-200 answers on the tap path are all "offline".** 429 (60/min shared by the user's
  hubs and app) and 5xx become `armut`; the boot re-check of N cards can trip the limiter.
  Fix: FW branches on status, honours Retry-After; P keys the limiter per hub.
- **403 never refreshes the token** (only 401 does): after an unbind, fault uploads and OTA
  checks fail forever until reboot. Fix: FW treats 403 from hub endpoints as "refresh once";
  P revokes the token on unbind/delete.
- **Fault catalogue truncated on upload.** 22 remedies contain `;`, the marker's record
  separator; the portal cuts them at the first `;`. Fix: reword, and make tools/faults.py and
  the build refuse `;`/`|` in any field.
- **`age_s` wrong for events from earlier boots.** The previous boot's uptime is zeroed at
  boot before it is used. Fix: capture it first; send `boot`+`uptime_s` and let the portal
  derive a lower bound.
- **Active fault list differs between channels** (report capped at 4, events endpoint 8;
  severities computed from different catalogues in the two listeners). Fix: one cap, `forHub`
  in both.
- **`play-track` matches by file suffix** (`/{id}.mp3`): content and custom tracks with the
  same id collide; `current_track_id` is actually a path. Fix: P sends the path (it has it).
- **`delete-figure` / `sync-figure` value case.** P accepts lowercase hex; FW compares against
  uppercase file names. Fix: P normalises with `NfcId::normalize`.
- **Chat / command parsing with `strstr`.** Escaped quotes and `\uXXXX` (Turkish letters) break
  chat text; the parser routes on substrings of the whole frame. Fix: FW parses with ArduinoJson.
- **Device chat send is Firebase-only on the portal** (console feature) → always 401. Drop it or
  move the route.

### Portal
- `GET /api/units/{nfc}/contents` and the content↔unit bind/unbind endpoints check neither
  card ownership nor (for unbind) content ownership: any app user can strip content from any
  wildcard card.
- Shopify refund revokes content without `DeviceContentSync`; admin user deletion deletes
  custom tracks without sync; admin user bind/unbind of a card sends no sync either.
- Webhook event stored but job dispatch failure leaves it `received` forever (retry only
  accepts `failed`); claim redeem has no lock; grant is idempotent so a second purchase is not
  recorded and a refund of the first revokes what the second paid for; refund matching breaks
  when a SKU changes; SKU not unique.
- Figure type change / card reassignment on import do not re-check cards and contents;
  figure delete orphans its contents (figure_id null, no SKU); content delete orphans episodes.
- Pivots without unique constraints (hub_user, unit_user, content_user); string `users.id`
  against bigint pivot columns (blocks MySQL); `User::$fillable` includes `id` and `role`.
- `ota_status` stays stale after a USB flash (check updates the version only);
  `prompt_pack_state` updated only by the check, not by reports; the report carries no
  firmware version.
- Firmware release delete removes the file while hubs may be mid-download; prompt pack derive
  can delete files a device is staging.
- Rate limits: none on `/auth/register`, `/auth/social-login`, `/api/register`; legacy
  `/api/login`, `/api/register`, `/api/logout` (500 for Firebase users) still exposed;
  `HybridAuth` accepts any Sanctum token regardless of name; tokens never expire.
- Logging of e-mails, chat text before authorisation, claim codes; exception messages returned
  to clients.
- Reverb: client events relayed unchecked, allowed origins `*` (a stolen token can inject fake
  reports; the listener only checks subscription).
- Error response shapes (four different), no pagination on `/hubs`, `/units`, `/contents`,
  `/custom-tracks`; `/contents/available` one query per content; `Unit::$with` still loads the
  whole content tree on the API side.

### Firmware
- Unpaired or unregistered toy says `armut` ("I am not online") while online: add a fault and
  prompt for "not linked to an account" (token 404).
- Bring-up waits forever for charge current on a full cell / without USB, never sleeps
  meanwhile, leaves no record on failure; after bring-up the inactivity timeout is 30 s until
  the next reboot.
- A card from another toy is accepted: its manifests play cache-first and its download queue
  resumes. Write a `/.device` marker with the uuid, wipe the cache when it differs.
- Card removed at runtime: no detection, reboot needed (known).
- `uid_mappings` in NVS is one unbounded JSON string in a 20 KB partition; two code paths
  erase all of NVS on `NO_FREE_PAGES`.
- `hubid` prints the MAC byte-reversed; QR output is a stub.
- Open-network credentials never mirrored (see A.8).
- Docked toy whose router password changed restarts every ~15 min and says hello + "I cannot
  connect" each time, all night: cap the restarts, quiet hours.
- Figure left on the pins: wake/replay loop on battery (known, untested).
- Production console still offers `sdformat` (no confirmation), `download`/`addfile`/
  `deletefile` (any URL to any path), `ota url` (any image, attacker-supplied hash), `testauth`
  (sends the token with `setInsecure`), `config` (prints the PIN).
- Assets: sha256 verified only when the manifest sends one; empty checksum = unverified HTTP
  content. Refuse empty checksums; portal always sends one.
- No SNTP: TLS certificate dates are never checked; no image signing / secure boot / flash
  encryption (token and WiFi password readable over USB); debug build (`-Og`).
- Asset downloads without Range (a retry restarts the file); downloads refuse 301/302 and
  chunked encoding (the nginx port-80 contract is not committed anywhere in the portal repo).
- `faults.last.d` escapes `"` but not `\` (one backslash breaks the whole report frame).
- Inventory caps a figure at 60 tracks without a truncated flag; Reverb max message size
  10,000 bytes is reachable by a large figure.

## C. Docs that no longer match the code
- P docs/OTA.md: "re-checks right away on 403" (it re-checks after 10 min); "fields never
  contain ;" (22 do); pin kept on failure without the loop note. FW docs/FAULTS.md: "entries
  stay until 200" (dropped on 4xx), "clearing is reported" (it is not), no mention of the
  4-entry cap. P DEVICE-COMMANDS.md: `play-track` semantics, `reboot`, ota state `prompts`.
  FW tools/sdcard-prepare.sh comment lists folders the firmware no longer creates.
  FACTORY_BRINGUP.md tells technicians to use bench-only commands and to expect Chem ID 0x418,
  which no board has. FW ReverbClient.cpp:598 stale cadence comment.

## D. Verified non-issues
- `/broadcasting/auth` works for devices in production (200 in nginx, subscription succeeds);
  the CSRF inference in the backend audit does not hold.
- Pack checksum algorithm: tools/sounds/gen_prompts.py, P PromptPackChecksum and the firmware
  agree (checked on the generated pack).
- TTPROMPTS1 marker and resources/device-faults.json match the firmware.

## E. Manufacturing pipeline, draft (I = exists, M = manual today, X = missing)
1. Fit the cell (not full), connect USB-C. M
2. `esptool erase-flash`, then write `tilkietalkie-<ver>-usb.bin` at 0x0. I (image) / M (erase
   not documented; needed to clear bench NVS state: endpoint override, gauge bus release,
   volume/brightness). Open: verify whether the merged image overwrites NVS (0x9000-0xDFFF).
3. Bring-up until green (gauge provisioning, charge, current calibration). I (state machine)
   / X (machine-readable result, re-run on a production image).
4. Gauge chemistry: bqStudio session once, golden data-flash image, replay per unit, seal. X
   (docs/GAUGE-CHEMISTRY.md); FACTORY_BRINGUP's 0x418 check is wrong until then.
5. Read identity: uuid (decimal MAC) + correctly ordered MAC + serial + hardware revision. I
   (`hubid`, wrong byte order) / X (serial, per-unit hardware rev).
6. Register the hub on the portal with its secret: `POST /api/factory/hubs` + `ft enroll`. X
7. Self-test with pass/fail: LEDs, buttons, NFC read, pogo, speaker (loopback or current
   delta), jack, charger/gauge/PD, SD read/write speed, WiFi scan against the factory AP, BLE
   advertising, sleep current. M (operator judgement with console commands) / X (`ft` protocol,
   JSON result, station script).
8. Pre-load the SD card with the current prompt pack and verify pack.json + file count on the
   device. I (`tools/sdcard-prepare.sh`, macOS, interactive) / X (batch mode, read-back,
   on-device verification that fails the station).
9. Factory clean: endpoint, gauge bus, WiFi, token, fault ring, volume, card cache. X
10. Ship mode with the delay bit, at the defined charge. X
11. Label: QR with uuid, PoP, serial. X
12. Customer pairing: BLE provisioning with the per-unit PoP, binding tied to possession,
    "not linked" prompt. I (BLE, shared PoP) / X (per-unit PoP, possession, prompt).

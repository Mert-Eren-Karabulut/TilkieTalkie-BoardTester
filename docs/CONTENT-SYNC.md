# Content sync: how a change on the portal reaches the toys

Firmware 1.2.14 + portal `DeviceContentSync`. Related: docs/VOICE-PROMPTS.md (prompts),
portal `docs/DEVICE-COMMANDS.md` (commands and events).

## What the card holds

- `/.requestmanager/manifests/<unit uid>.json`: the portal's manifest of one **unit** (one
  physical figure/card), as last downloaded: figure, contents → episodes → tracks,
  custom tracks, `asset_manifest`, `manifest_checksum`.
- `/.requestmanager/pending_manifests/<unit uid>.json`: the same while its tracks are still
  downloading. Promoted to `manifests/` when the last track lands (or fails for good).
- `/assets/...`: the audio, named by the portal's `storage_path`
  (`assets/contents/{content}/{episode}/{track}.mp3`, `assets/custom_tracks/{id}.mp3`).
  One copy per track whatever the number of units or figures that reference it; a track
  is deleted only when no cached manifest references it any more.
- NVS: unit uid → figure id (for the portal's inventory).

Why per unit and not per figure: the portal's manifest endpoint is `/units/{uid}` because
wildcard figures carry per-unit content (`content_unit`, `custom_track_unit`). For a normal
figure every unit of it gets the same manifest; two cards of the same figure cost two
small JSON files and no duplicate audio.

## When the toy re-reads a manifest

1. **Tap, online** — always. Cached tracks play at once; the manifest is fetched in the
   background and compared by `manifest_checksum`. New or changed tracks are downloaded,
   removed ones deleted, and the playlist is merged without interrupting what plays.
   A track whose file was replaced on the portal (same path, new sha256) is re-downloaded;
   if it is the one playing right now it is refreshed on the next tap.
2. **`sync-figure <uid>` command** — pushed by the portal whenever content that a unit's
   manifest depends on changes (`App\Services\DeviceContentSync`, wired into the admin and
   mobile endpoints that create/update/delete tracks, episodes, contents, custom tracks,
   and attach/detach them). Every hub of every user who owns the unit gets one command per
   affected unit. The toy ignores units it has not cached; otherwise it re-fetches like a
   tap would, silently (no prompt, no playback for a figure that is not on the pins).
3. **Periodic re-check** — 3–4 minutes after boot and every 12 hours: every cached unit is
   asked with `If-None-Match: "<manifest_checksum>"`; the portal answers `304` (no body)
   when nothing changed, `200` with the manifest otherwise. One request at a time, behind
   anything a tap needs. Covers units nobody bound to a user, and pushes missed while
   offline.

Downloads of changed content happen whenever the toy is online (battery or dock), one
file at a time, with the story playlist waiting for the next track at its end if needed.

## Manifest answers (firmware 1.2.20+)

| status | meaning on the toy |
|---|---|
| 200 | manifest, applied as above |
| 304 | (re-check only) unchanged |
| 404 | no such unit: tap → `201 FIGURE_UNKNOWN` (elma); a cached copy is **kept** |
| 410 `{"revoked": true}` | the card was deleted on the portal: its manifest, pending manifest, the assets no other cached card references and their queued downloads are **purged**, the inventory is re-sent; a tap says elma |
| 429 | throttled: every portal request pauses for `Retry-After` (default 60 s) and is re-sent (twice at most); not "offline" |
| 5xx | a tap is answered at once (the cache plays; nothing cached → armut); background re-checks are re-sent after 30 s, 2 min, 10 min |
| 403 | the device token is refreshed (at most once per 10 min) and the request retried once |

An asset without a `checksum` is **not downloaded** (`204 DOWNLOAD_FAILED`, detail `nosha`); a
copy already on the card stays playable. Queued entries without a checksum left by older
firmware are dropped. The portal always sends checksums (PROTOCOL-V2 §8).

## Interrupted downloads

**Range resume.** A transfer cut by a link drop, a stall or a reboot keeps its `<path>.tmp`
(the boot sweep of orphaned `.tmp` files keeps those of queued downloads). The next attempt
sends `Range: bytes=<tmp size>-`:
- `206` with `Content-Range: bytes <tmp size>-<last>/<total>` (`last + 1 == total`): the rest is
  appended; the SD writer hashes the prefix first, so the sha256 covers the whole file, and the
  final size must equal `total`;
- `200`: the server ignored the Range: the `.tmp` is truncated and the whole file written;
- `416`, or any other `Content-Range`: the `.tmp` is deleted and the download restarts from
  zero at once (not counted as a failed attempt).
A checksum mismatch, an oversize file, a full card or a write error deletes the `.tmp`; so does
giving up on the file or cancelling it.

A reboot (update, button) in the middle of a figure download used to leave the pending
manifest orphaned: the queued downloads resumed, but nothing promoted the manifest, so the
figure was invisible to the inventory and "not cached" for an offline tap. Now
`recoverPendingManifests()` runs after the card is mounted: each pending manifest gets its
tracker back and the missing files are (re)queued.

## What the portal sees

- `device.status.updated` → `files`: `status` (`syncing` while the download queue is not
  empty), `pending` (queued files), `downloading` (file name in flight), `progress` (0–100,
  -1 when idle), `sd_remaining` (MB).
- `device.files.updated` (one per figure, after `list-files`, and pushed by the toy whenever
  a figure starts downloading or is promoted/deleted): `figure.state` is `downloading`
  (pending manifest), `partial` (final manifest, some tracks missing) or `ready`.
- `last_error` is cleared when a figure plays, when WiFi is back (portakal), and by the
  reboot that ends an update (karpuz).

## Storage over port 80: the nginx contract

Story tracks (`/storage/tracks/...`, `/storage/contents/...`, `/storage/custom_tracks/...`, as the
manifest's `url` says) and prompt pack files (`/storage/prompts/...`) are fetched by AsyncTCP
over **plain HTTP on port 80** (no TLS client on that path; integrity comes from the sha256 in
the TLS manifest). The portal's nginx must, for `http://portal.tilkietalkie.com/storage/`:
1. answer on port 80 **without** redirecting to HTTPS: a `301`/`302` is refused by the toy (the
   file stays queued, the figure never completes; the old code saved the redirect page);
2. send a `Content-Length` and **no** `Transfer-Encoding: chunked` (refused) - i.e. serve the
   file statically (`sendfile`), not through PHP;
3. honour `Accept-Encoding: identity` (no gzip of mp3/json);
4. support single byte ranges: `Range: bytes=N-` → `206` with `Content-Range`, `416` when N is
   past the end (nginx's static module does this by default);
5. close the connection after the response (`Connection: close` from the toy);
6. keep `/storage/prompts/` on the same rules as the tracks (a redirect there shows up as the
   pack's files staying "queued").
The `https://` URLs in manifests are rewritten to `http://` by the firmware for this path; the
API, the firmware image download and Reverb stay on TLS.

## Inventory (`client-device-files`)

One message per cached figure, `seq` 1..`total` (`total: 0, figure: null` for an empty card),
plus `sd`. A figure lists at most 60 tracks; with more, `figure.truncated: true` (always a
boolean). A figure whose track list does not fit one frame (every frame < 9,000 bytes; Reverb
drops > 10,000) is sent as several messages with the same `seq`/`total` and top-level
`part` (1-based) and `parts` (≤ 32); each part repeats `figure.uid`, `id`, `name`, `bytes`,
`state`, `truncated` and carries only its slice of `tracks`. The portal concatenates the parts of
the same `seq`/`uid` in part order. A figure that fits one frame carries no `part`/`parts`.
Track and figure names are cut at 120 bytes on a UTF-8 boundary.

## Not done yet

- The portal does not store the inventory the toys send; it is shown live only. Storing it
  (hub × unit) would show "what is on this toy" while it is offline and let the push
  target exactly the hubs that cache a unit instead of all hubs of its owners.
- A content change is not applied to a figure while it plays; it lands on the next tap.

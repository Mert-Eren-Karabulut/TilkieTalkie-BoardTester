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
- NVS: unit uid → figure id, the 48 most recently used units (`requestmgr/uid_lru`; the card's
  manifest carries the same id and is what the inventory reads). The old unbounded
  `uid_mappings` object is migrated once.
- `/.device`: the uuid of the toy this cache belongs to. A card that names another toy is
  wiped on mount (stories, manifests, queue; the prompt pack stays), 306 CARD_FOREIGN.

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

## Interrupted downloads

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

## Not done yet

- The portal does not store the inventory the toys send; it is shown live only. Storing it
  (hub × unit) would show "what is on this toy" while it is offline and let the push
  target exactly the hubs that cache a unit instead of all hubs of its owners.
- A content change is not applied to a figure while it plays; it lands on the next tap.

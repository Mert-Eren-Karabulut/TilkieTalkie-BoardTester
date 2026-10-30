# Runtime: tasks, the loop, and what must never block

The rule: the Arduino loop (core 1, priority 1) owns the application state (audio control,
figures, faults, NVS, the portal client) and must finish every pass in well under 100 ms; the
things that wait on the outside world (sockets, TLS, SD card, the battery bus) run on their own
tasks and talk to the loop through queues and atomics. `diag on` prints the per-section loop
timing every 10 s (the print itself costs ~250 ms per table over USB-CDC); `timing` prints it
once; `tasks` prints the FreeRTOS run-time stats and stack high-water marks.

## Tasks
| task | core | prio | stack | job | yields |
|---|---|---|---|---|---|
| loopTask | 1 | 1 | 10 KB | setup/loop: everything below "Loop" | `delay(1)` per pass; task WDT 120 s |
| AudioPlayback | 1 | 12 | 8 KB | MP3 decode into I2S DMA (16×128 frames ≈ 46 ms) under `audioMutex` | 1 ms between slices |
| audio_rd | 0 | 6 | 4 KB | fills the 128 KB PSRAM ring with 4 KB SD reads | notify / 20 ms |
| reverb | 0 | 5 | 12 KB | the WebSocket: frames, keepalive (ping 15 s, pong 3 s, 2 misses), TLS reconnect (2→60 s backoff), channel auth HTTPS, outbox, the report the loop built | 5 ms |
| SDWriteTask | 0 | 5 | 6 KB | per download: drains the 4 MB PSRAM ring to the card in 16 KB writes, sha256 | 50 ms wait |
| ota | 0 | 5 | 8 KB | firmware download (docked, idle) | blocking HTTP |
| req | 0 | 4 | 8 KB | synchronous HTTPS requests (8 s timeout), on demand | exits when idle |
| battery | 0 | 4 | 4 KB | gauge + charger poll every 1 s, ~190 ms of bit-banged I2C (busy-wait) | 100 ms |
| led | 1 | 3 | 3 KB | 50 Hz render over RMT | 20 ms |
| sd_maint | 0 | 2 | 8 KB | deferred deletes/writes, free-space count, inventory | queue |
| async_tcp / wifi / lwIP / events | 0 | 18–23 | | IDF networking | |

## Loop sections (TT_TIMED names) and their budget
ft, battery (no-op once the poll task runs), charger (occasional I2C writes under the bus lock),
files (download start/finish: SD rename + callback), audio (prompt handling, volume NVS write),
wifi, reverb (hand-offs from the socket task + building the report), ota, prompts, faults
(flash save at most every 30 s, errors at once), led, buttons, nfc (70 ms reads only while a
figure sits unread on the pins), request (figure callbacks: manifest parse + SD stat per
asset, ~150 ms at a tap), chargeux, netstart, nethealth, ux, battux, periph (card probe every
5 s, paused during playback and downloads; codec retry), sleep.

Measured 2026-10-30 on the bench (1.3.19 + the task split), story playing and a 4 MB track
downloading at 4.4 Mbps at the same time: worst loop gap 250 ms (the tap), reverb section ≤ 16 ms,
socket subscribed throughout, LED frame gap ≤ 60 ms.

## Hand-offs
- WebSocket task → loop: device commands and chat messages (queue of 16, `Handoff`), fault
  flags (`_deferredFaults`: WS_DISCONNECTED, WS_AUTH_FAILED raise/clear). Executed in
  `ReverbClient::update()` on the loop.
- Loop → WebSocket task: the device report (built once a second at most, copied into
  `reportBuffer`, sent by the task; `set-report-interval 1` from the hub page while it is open).
- Audio task → loop: faults through `queueFault` (the loop *tries* the mutex for 2 ms), prompt /
  playlist ended as atomics. The loop never waits on `audioMutex` for a read: `getCurrentTrack()`
  tries 20 ms and otherwise returns its last answer. Control calls (stop, next, seek) still take
  the mutex and can wait on the SD card (`parkReader` up to 5 s): acceptable for a button press,
  a known cost.
- sd_maint → WebSocket task: the file inventory (`client-device-files`) through the outbox.

## Shared resources
- `audioMutex` (recursive): the audio task holds it for each decode slice, including SD waits.
  Only control calls may block on it from the loop.
- SD_MMC / FatFs (one volume lock, 10 s timeout): the audio reader, the download writer, the
  maintenance task and the loop's file operations. Keep loop-side SD work to open/stat; scans
  (free space, inventory, sweeps) belong on sd_maint.
- The battery bus lock (`busLock`): the poll task per transaction; data-flash sequences hold it
  for hundreds of ms. Getters read cached values.
- NVS writes stall both cores' caches (no flash auto-suspend): write rarely, never per pass.

## What to watch when adding code to the loop
No `delay()` over a few ms, no synchronous HTTP/TLS, no `SD_MMC.open` in a per-pass path, no
NVS write in a per-pass path, no `portMAX_DELAY` on a mutex another task holds across I/O. If
it must wait, give it a task and a queue.

## Radio reset
`WiFiProvisioningManager::resetRadio()` turns the WiFi driver off and on. It must never run
while a task is inside a socket read: the request worker crashed in `pbuf_free` through a
dangling pointer when the driver was deinitialised under its HTTPS exchange (core dump,
2026-10-30). The reset therefore first pauses the WebSocket task (`ReverbClient::pauseSocket`,
3 s) and takes the request manager's HTTP lock (`lockHttp`, 12 s: a request times out in 8 s),
disconnects the socket, and only then touches the driver. The health check in main.cpp never
resets while the socket is connected. To read a crash: `esptool read_flash 0xfe0000 0x9000
core.bin`, then `espcoredump.py --chip esp32s3 info_corefile -t raw -c core.bin
.pio/build/<env>/firmware.elf` with `tool-xtensa-esp-elf-gdb/bin` on the PATH.

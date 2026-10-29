# Releasing firmware

1. **Build**: `tools/release.sh <major.minor.patch>` bumps `CONFIG_APP_PROJECT_VER` in
   `sdkconfig.esp32-s3-devkitc-1` (the single source of the version; the portal derives
   `version_code = major*10000 + minor*100 + patch`), builds the production env and drops
   `releases/tilkietalkie-<ver>.bin` with its sha256. Then `tools/usb-image.sh <ver>` builds
   `releases/tilkietalkie-<ver>-usb.bin` (bootloader + partition table + otadata + app, flash at
   0x0) for the station and the web flasher. Commit the sdkconfig bump ("Release <ver>").
2. **Publish on beta** (run from your own terminal; the portal's `tilkie:firmware-release`
   command registers the release and its USB image):
   ```
   scp releases/tilkietalkie-<ver>.bin releases/tilkietalkie-<ver>-usb.bin tilkietalkie-portal@portal.tilkietalkie.com:~/releases/
   ssh tilkietalkie-portal@portal.tilkietalkie.com 'cd ~/htdocs/portal.tilkietalkie.com && php artisan tilkie:firmware-release ~/releases/tilkietalkie-<ver>.bin --device="Adem v7" --channel=beta --no-nudge --usb-image=$HOME/releases/tilkietalkie-<ver>-usb.bin --notes="..." && cp ~/releases/tilkietalkie-<ver>-usb.bin public/flash/tilkietalkie-usb.bin'
   ```
   then bump `version` in the portal's `public/flash/manifest.json` and push `develop` → `main`
   (main deploys). The bench unit on the beta channel picks it up at its next check.
3. **Promote to stable** once the bench run is clean (same terminal):
   ```
   ssh tilkietalkie-portal@portal.tilkietalkie.com 'cd ~/htdocs/portal.tilkietalkie.com && php artisan tinker --execute "\$r = \App\Models\FirmwareRelease::where(\"version\", \"<ver>\")->firstOrFail(); \$r->channel = \"stable\"; \$r->save(); echo \$r->version.\" -> \".\$r->channel.PHP_EOL;"'
   ```
   Hubs on stable take it at their next check (not mandatory unless set so on the release).
4. Record it in SHIP-CHECKLIST.md.

Release images are always the production env. A bench board gets `pio run -e bench -t upload`
instead (bench console commands, docs/CONSOLE.md); never publish that image.

## Flashing a unit over USB
The station (portal `/factory`, Chrome with WebSerial) flashes the current USB image itself
(docs/MANUFACTURING.md). Without the station: `https://portal.tilkietalkie.com/flash/` in
Chrome (Install, pick the `usbmodem…` port; WiFi pairing and the account binding survive, NVS
is kept), or `esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX write-flash 0x0
tilkietalkie-<ver>-usb.bin`. A unit left dark in the ROM flasher stub after a flash needs a
reset: `esptool --port <port> --after hard_reset chip_id`. The card needs `/sounds` (the prompt
pack, `tools/sdcard-prepare.sh`).

First boot on USB runs the bring-up (gauge provisioning, the learned gauge image, current
calibration; LED colours in docs/MANUFACTURING.md).

## Factory screen password (production)
From a zsh terminal, without echoing the value anywhere:
```
read -s 'FP?New factory password: '; echo; ssh tilkietalkie-portal@portal.tilkietalkie.com "cd ~/htdocs/portal.tilkietalkie.com && if grep -q '^FACTORY_PASSWORD=' .env; then sed -i 's|^FACTORY_PASSWORD=.*|FACTORY_PASSWORD=$FP|' .env; else echo 'FACTORY_PASSWORD=$FP' >> .env; fi && php artisan optimize:clear >/dev/null && echo 'factory password set'"; unset FP
```
Letters and digits only. An empty value disables the factory login.

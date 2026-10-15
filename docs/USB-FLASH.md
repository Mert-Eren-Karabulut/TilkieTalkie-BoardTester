# Updating a device over USB (no OTA yet, or a bricked image)

For a unit that runs firmware older than 1.1.x (no OTA), or that cannot boot. Needs only
Chrome or Edge and a data USB-C cable.

1. Build the image: `tools/release.sh <ver>` then `tools/usb-image.sh <ver>` →
   `releases/tilkietalkie-<ver>-usb.bin` (bootloader + partition table + otadata + app, flash at 0x0).
2. Publish: copy it to the portal as `public/flash/tilkietalkie-usb.bin` and bump `version`
   in `public/flash/manifest.json` (committed). The sound pack: `zip -r sounds.zip sounds`
   from `sdcard/` → `public/flash/sounds.zip`.
   ```
   scp releases/tilkietalkie-<ver>-usb.bin portal.tilkietalkie.com:~/htdocs/portal.tilkietalkie.com/public/flash/tilkietalkie-usb.bin
   ```
3. The person with the device opens https://portal.tilkietalkie.com/flash/ in Chrome, plugs
   the toy in, clicks Install, picks the `usbmodem…` port. About a minute. WiFi pairing and
   the account binding survive (NVS is not erased).
4. The card needs `/sounds` (prompts). Copy the unzipped `sounds` folder to the card root.

Without a browser: `esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX write-flash 0x0 tilkietalkie-<ver>-usb.bin`
(`pip install esptool`).

First boot on a board whose gauge was never calibrated runs the bring-up (blue → green on USB
power with a battery that is not full; see FACTORY_BRINGUP.md).

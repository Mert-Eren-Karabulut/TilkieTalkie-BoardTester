#!/bin/bash
# Build a single "flash at 0x0" image (bootloader + partition table + OTA data + app) from the
# current build, for USB flashing with the web flasher (portal /flash/) or esptool:
#   tools/usb-image.sh <major.minor.patch>          -> releases/tilkietalkie-<ver>-usb.bin
#   esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX write-flash 0x0 releases/tilkietalkie-<ver>-usb.bin
# Run tools/release.sh <ver> first so the build matches the version.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$HOME/.platformio/penv/bin/python"
ENV=esp32-s3-devkitc-1
B="$ROOT/.pio/build/$ENV"
VERSION="${1:?usage: tools/usb-image.sh <major.minor.patch>}"
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
for f in "$B/bootloader.bin" "$B/partitions.bin" "$B/firmware.bin" "$BOOT_APP0"; do
  [ -f "$f" ] || { echo "missing $f (build first)" >&2; exit 1; }
done
# the app image must carry the requested version
APPVER=$("$PY" - "$B/firmware.bin" <<'PYEOF'
import sys
b = open(sys.argv[1], "rb").read()
print(b[0x30:0x50].split(b"\0")[0].decode())
PYEOF
)
[ "$APPVER" = "$VERSION" ] || { echo "build is version $APPVER, not $VERSION - run tools/release.sh $VERSION first" >&2; exit 1; }
mkdir -p "$ROOT/releases"
OUT="$ROOT/releases/tilkietalkie-$VERSION-usb.bin"
"$PY" -m esptool --chip esp32s3 merge-bin -o "$OUT" \
  0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin" >/dev/null
echo "image   $OUT"
echo "version $VERSION"
echo "size    $(stat -f%z "$OUT") bytes"
echo "sha256  $(shasum -a 256 "$OUT" | cut -d' ' -f1)"

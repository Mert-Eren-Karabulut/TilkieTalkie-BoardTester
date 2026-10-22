#!/usr/bin/env bash
# Build a release image: bumps CONFIG_APP_PROJECT_VER (the single source of the firmware
# version, embedded in the app descriptor and parsed by the backend upload), builds, and drops
# the .bin with its sha256/size into releases/ ready for the admin Firmware page.
#
#   tools/release.sh 1.2.0            # build release 1.2.0
#   tools/release.sh 1.2.0 --upload   # ...and flash it over USB too
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIO="$HOME/.platformio/penv/bin/pio"
ENV=esp32-s3-devkitc-1
CFG="$ROOT/sdkconfig.$ENV"
VERSION="${1:-}"
# Strict major.minor.patch, each 0-99 without leading zeros, no prefix or suffix: the same rule
# as the firmware (src/Version.h, which also refuses the build) and the portal, which derives
# version_code = major*10000 + minor*100 + patch from the image.
PART='(0|[1-9][0-9]?)'
if ! [[ "$VERSION" =~ ^$PART\.$PART\.$PART$ ]]; then
  echo "refusing version '$VERSION': must be major.minor.patch, each 0-99 (e.g. 1.2.20; not v1.2.20, 1.2, 1.2.100)" >&2
  echo "usage: tools/release.sh <major.minor.patch> [--upload]" >&2
  exit 1
fi
IFS=. read -r MA MI PA <<<"$VERSION"
CODE=$((MA * 10000 + MI * 100 + PA))

sed -i '' -E "s/^CONFIG_APP_PROJECT_VER=\"[^\"]*\"$/CONFIG_APP_PROJECT_VER=\"$VERSION\"/" "$CFG"
grep -q "^CONFIG_APP_PROJECT_VER=\"$VERSION\"$" "$CFG" || { echo "could not set CONFIG_APP_PROJECT_VER in $CFG" >&2; exit 1; }

cd "$ROOT"
"$PIO" run -e "$ENV" >/dev/null 2>&1 || "$PIO" run -e "$ENV"   # a sdkconfig change sometimes needs a second configure pass
BIN="$ROOT/.pio/build/$ENV/firmware.bin"
mkdir -p "$ROOT/releases"
OUT="$ROOT/releases/tilkietalkie-$VERSION.bin"
cp "$BIN" "$OUT"
python3 - "$OUT" <<'PY'
import sys, struct, hashlib
p = sys.argv[1]; b = open(p, "rb").read()
assert b[0] == 0xE9 and struct.unpack("<I", b[0x20:0x24])[0] == 0xABCD5432, "not an ESP32 app image"
ver = b[0x30:0x50].split(b"\0")[0].decode()
print(f"image   {p}")
print(f"version {ver}")
print(f"size    {len(b)} bytes")
print(f"sha256  {hashlib.sha256(b).hexdigest()}")
PY
echo "code    $CODE  (what the backend derives from $VERSION)"
echo "Next: commit the sdkconfig bump, then upload the .bin on the admin Firmware page (device model, channel, rollout)."
if [ "${2:-}" = "--upload" ]; then
  "$PIO" run -e "$ENV" -t upload --upload-port "${UPLOAD_PORT:-/dev/cu.usbmodem21201}"
fi

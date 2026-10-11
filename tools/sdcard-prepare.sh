#!/bin/bash
# Prepare an SD card for a TilkieTalkie: fresh FAT32 + the sound pack from sdcard/.
#
#   1. Put the card in a reader, then find it:   diskutil list external
#   2. tools/sdcard-prepare.sh /dev/diskN          (the whole disk, not a partition like disk4s1)
#
# Everything on the card is erased. The firmware creates its own folders (audio, temp, logs,
# images, .requestmanager) on first boot; stories are downloaded per figure.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK="${1:?usage: tools/sdcard-prepare.sh /dev/diskN   (see: diskutil list external)}"
LABEL=TILKIE

if [ ! -d "$ROOT/sdcard/sounds/tr" ]; then
  echo "sdcard/sounds/tr is missing - run tools/sounds/gen_prompts.py first" >&2
  exit 1
fi
INFO="$(diskutil info "$DISK")"
if ! echo "$INFO" | grep -qE "Removable Media: *Removable|Protocol: *(USB|Secure Digital|SD)"; then
  echo "refusing: $DISK does not look like a removable card" >&2
  echo "$INFO" | grep -E "Device / Media Name|Disk Size|Protocol|Removable" >&2
  exit 1
fi
echo "$INFO" | grep -E "Device / Media Name|Disk Size|Protocol"
read -r -p "Erase $DISK and write the sound pack? [y/N] " yn
[ "$yn" = "y" ] || exit 1

diskutil eraseDisk FAT32 "$LABEL" MBRFormat "$DISK"
MP="/Volumes/$LABEL"
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -d "$MP" ] && break; sleep 1; done
[ -d "$MP" ] || { echo "card did not mount at $MP" >&2; exit 1; }
cp -R "$ROOT/sdcard/." "$MP/"
rm -rf "$MP/.fseventsd" "$MP/.Spotlight-V100" "$MP/.Trashes" 2>/dev/null || true
find "$MP" -name '._*' -delete 2>/dev/null || true
sync
echo "written:"; find "$MP/sounds" -type f | sed "s|$MP/||" | sort
diskutil eject "$DISK"
echo "done - put the card in the toy"

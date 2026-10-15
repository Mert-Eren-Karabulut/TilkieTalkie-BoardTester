#!/bin/bash
# Prepare an SD card for a TilkieTalkie: fresh FAT32 + the sound pack from sdcard/.
#
#   1. Put the card in a reader, then find it:   diskutil list external physical
#   2. tools/sdcard-prepare.sh /dev/diskN [prompts.zip]
#      /dev/diskN: the whole disk, not a partition like disk4s1.
#      prompts.zip (optional): a prompt pack downloaded from the portal's Firmware page
#      ("Download" on the pack), laid out as sounds/...; without it the repo's sdcard/sounds
#      (tools/sounds/gen_prompts.py output) is written. The pack's pack.json goes along, so the
#      toy reports which pack it carries and only downloads a pack when the portal pairs a
#      different one with its firmware.
#
# Everything on the card is erased. The firmware creates its own folders (audio, temp, logs,
# images, .requestmanager) on first boot; stories are downloaded per figure.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK="${1:?usage: tools/sdcard-prepare.sh /dev/diskN [prompts.zip]   (see: diskutil list external)}"
PACK_ZIP="${2:-}"
LABEL=TILKIE

SRC="$ROOT/sdcard"
if [ -n "$PACK_ZIP" ]; then
  [ -f "$PACK_ZIP" ] || { echo "pack zip not found: $PACK_ZIP" >&2; exit 1; }
  SRC="$(mktemp -d)"
  unzip -q "$PACK_ZIP" -d "$SRC"
  [ -f "$SRC/sounds/pack.json" ] || { echo "$PACK_ZIP has no sounds/pack.json - not a prompt pack from the portal" >&2; exit 1; }
  echo "pack: $(grep -o '"checksum": *"[0-9a-f]*' "$SRC/sounds/pack.json" | head -1 | grep -o '[0-9a-f]*$' | cut -c1-12) from $PACK_ZIP"
elif [ ! -f "$SRC/sounds/pack.json" ]; then
  echo "sdcard/sounds/pack.json is missing - run tools/sounds/gen_prompts.py first" >&2
  exit 1
fi
INFO="$(diskutil info "$DISK")"
# A real card shows up as a USB reader or a built-in SD slot; disk images and internal disks are refused.
if echo "$INFO" | grep -qE "Protocol: *Disk Image|Virtual: *Yes|Internal: *Yes|Protocol: *(PCI|Apple Fabric)"; then
  echo "refusing: $DISK is not a physical card (disk image or internal disk)" >&2
  echo "$INFO" | grep -E "Device / Media Name|Disk Size|Protocol|Removable|Virtual|Internal" >&2
  echo "find the card with: diskutil list external physical" >&2
  exit 1
fi
if ! echo "$INFO" | grep -qE "Protocol: *(USB|Secure Digital|SD)" || ! echo "$INFO" | grep -qE "Removable Media: *Removable"; then
  echo "refusing: $DISK does not look like a removable card" >&2
  echo "$INFO" | grep -E "Device / Media Name|Disk Size|Protocol|Removable" >&2
  echo "find the card with: diskutil list external physical" >&2
  exit 1
fi
echo "$INFO" | grep -E "Device / Media Name|Disk Size|Protocol"
read -r -p "Erase $DISK and write the sound pack? [y/N] " yn
[ "$yn" = "y" ] || exit 1

diskutil eraseDisk FAT32 "$LABEL" MBRFormat "$DISK"
MP="/Volumes/$LABEL"
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -d "$MP" ] && break; sleep 1; done
[ -d "$MP" ] || { echo "card did not mount at $MP" >&2; exit 1; }
cp -R "$SRC/." "$MP/"
rm -rf "$MP/.fseventsd" "$MP/.Spotlight-V100" "$MP/.Trashes" 2>/dev/null || true
find "$MP" -name '._*' -delete 2>/dev/null || true
sync
echo "written:"; find "$MP/sounds" -type f | sed "s|$MP/||" | sort
[ -n "$PACK_ZIP" ] && rm -rf "$SRC"
diskutil eject "$DISK"
echo "done - put the card in the toy"

#!/usr/bin/env bash
# BoseFix32 — build release artefacts for ESP-Web-Tools distribution.
#
# For each target (s3, c3, c6) it produces:
#   webflasher/bosefix32-<tgt>-factory.bin   — merged bootloader+parts+app+fs
#   webflasher/bosefix32-<tgt>-firmware.bin  — app-only (for OTA over WiFi)
#   webflasher/bosefix32-<tgt>-littlefs.bin  — Web-UI image (for FS-OTA)
# Plus:
#   webflasher/manifest.json                 — esp-web-tools manifest with VERSION
#
# Run from project root.  Requires PlatformIO env active.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PIO_BUILD="/root/pio-build/bosefix32"
OUT="$ROOT/webflasher"
mkdir -p "$OUT"

# --- 1) Compile all envs + their LittleFS images --------------------------
# Pio's pre-build-hook (version_bump.py) bumpt build_number bei jedem
# `pio run`-Aufruf — daher liest dieses Script die Version ERST NACH dem
# letzten Bump (= nach buildfs). Sonst stuende im manifest.json ein
# kleinerer Build als in den .bin-Dateien drin (frueher Bug, beobachtet
# beim v0.4-Release).
"$HOME/.platformio/penv/bin/pio" run -e esp32 -e s3 -e c3 -e c6
"$HOME/.platformio/penv/bin/pio" run -e esp32 -e s3 -e c3 -e c6 -t buildfs

# Resolve final version after all bumps (single source of truth in version.h).
VERSION="$(grep -oE '"[0-9]+\.[0-9]+\.[0-9]+"' "$ROOT/src/version.h" | tr -d '"')"
echo ">>> BoseFix32 release build, version=$VERSION"

# --- 2) Per-target merge into single factory image -----------------------
ESPTOOL=( "$HOME/.platformio/penv/bin/pio" pkg exec --package "platformio/tool-esptoolpy" -- python -m esptool )

merge_target() {
  local tgt="$1" chip="$2" fsize="$3" spiffs_off="$4" boot_off="$5"
  local src="$PIO_BUILD/$tgt"
  local factory="$OUT/bosefix32-$tgt-factory.bin"

  echo ">>> Merging $tgt ($chip, $fsize, boot@$boot_off, spiffs@$spiffs_off)"
  "${ESPTOOL[@]}" --chip "$chip" merge_bin \
    -o "$factory" \
    --flash_mode dio --flash_size "$fsize" --flash_freq 80m \
    "$boot_off"   "$src/bootloader.bin" \
    0x8000        "$src/partitions.bin" \
    0x10000       "$src/firmware.bin" \
    "$spiffs_off" "$src/littlefs.bin"

  cp "$src/firmware.bin"  "$OUT/bosefix32-$tgt-firmware.bin"
  cp "$src/littlefs.bin"  "$OUT/bosefix32-$tgt-littlefs.bin"
}

# bootloader offset:
#   ESP32 (classic): 0x1000  (Boot-ROM springt dorthin)
#   S3 / C3 / C6 / S2 / C2 / C5 / C61 / H2 / P4: 0x0
# spiffs offsets must match the corresponding partition table:
#   partitions.csv      (16 MB) -> spiffs @ 0x610000
#   partitions-4mb.csv  ( 4 MB) -> spiffs @ 0x370000
merge_target esp32 esp32   4MB  0x370000  0x1000
merge_target s3    esp32s3 16MB 0x610000  0x0
merge_target c3    esp32c3 4MB  0x370000  0x0
merge_target c6    esp32c6 4MB  0x370000  0x0

# --- 3) Generate esp-web-tools manifest with current version -------------
cat > "$OUT/manifest.json" <<EOF
{
  "name": "BoseFix32",
  "version": "$VERSION",
  "funding_url": "https://github.com/tostmann/BoseFix32",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "bosefix32-esp32-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bosefix32-s3-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C3",
      "parts": [
        { "path": "bosefix32-c3-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C6",
      "parts": [
        { "path": "bosefix32-c6-factory.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

# --- 4) Summary -----------------------------------------------------------
echo
echo "=== Release artefacts (version $VERSION) ==="
ls -lh "$OUT"/*.bin "$OUT"/manifest.json
echo
echo "Public landing page:  https://install.busware.de/bosefix/"
echo "Deploy command (user triggers manually):"
echo "  rsync -avr webflasher/ 10.10.22.1:/var/www/install/bosefix/"

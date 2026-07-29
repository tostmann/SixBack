#!/usr/bin/env bash
# SixBack — build release artefacts for ESP-Web-Tools distribution.
#
# For each target (esp32, s3, s3-8mb, c3, c6, c5) it produces:
#   webflasher/sixback-<tgt>-factory.bin   — merged bootloader+parts+app+fs
#   webflasher/sixback-<tgt>-firmware.bin  — app-only (for OTA over WiFi)
#   webflasher/sixback-<tgt>-littlefs.bin  — Web-UI image (for FS-OTA)
# Plus:
#   webflasher/manifest.json                 — esp-web-tools manifest with VERSION
#
# Run from project root.  Requires PlatformIO env active.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PIO_BUILD="/root/pio-build/bosefix32"   # Build-Dir keep (path on disk, not user-visible)
OUT="$ROOT/webflasher"
mkdir -p "$OUT"

# --- 0) Release-tag handling ---------------------------------------------
# v0.7.6: tag-based versioning. When invoked with a release tag (arg or
# RELEASE_TAG env), all targets are built with the SAME version string
# baked in — eliminates the multi-target build drift (where the build
# counter advanced per `pio run`, so s3-firmware.bin reported a different
# version than c6-firmware.bin from the same release). Manifest.version
# also gets the tag, so /api/update/check compares cleanly.
#
# Usage:
#   bash scripts/build_release.sh                # dev build, counter as before
#   bash scripts/build_release.sh v0.7.6         # release build, tag-based
#   RELEASE_TAG=v0.7.6 bash scripts/build_release.sh   # same
RELEASE_TAG="${1:-${RELEASE_TAG:-}}"
if [ -n "$RELEASE_TAG" ]; then
  # strip leading 'v' if present (v0.7.6 -> 0.7.6) for both manifest and FW
  RELEASE_TAG_STRIPPED="${RELEASE_TAG#v}"
  export RELEASE_TAG  # picked up by scripts/version_bump.py
  echo ">>> Release build with tag $RELEASE_TAG (FW + manifest = $RELEASE_TAG_STRIPPED)"
else
  RELEASE_TAG_STRIPPED=""
  echo ">>> Dev build (no RELEASE_TAG) — FW + manifest use build counter"
fi

# --- 1) Compile all envs + their LittleFS images --------------------------
# Pio's pre-build-hook (version_bump.py) bumpt build_number bei JEDEM
# `pio run`-Aufruf. Beim Dev-Build (kein RELEASE_TAG) wandert der Counter
# pro Target weiter und der Counter im finalen version.h wird ins manifest
# geschrieben — multi-target drift bekannt aus v0.7.5 ([[reference-bosefix32-multitarget-build-drift]]).
# Beim Release-Build (RELEASE_TAG gesetzt) liest version_bump.py das
# RELEASE_TAG env und uebernimmt es als FW_VERSION_STRING ueberschreibt
# damit den Counter-Wert. Counter bumpt trotzdem (fuer git-snapshots
# und FW_VERSION_BUILD-Tracking), aber nicht user-facing.
#
# Reihenfolge buildfs vor firmware bleibt aus historischen Gruenden:
# (frueherer bug 2026-05-19, umgekehrt zeigte manifest.json +1 vs firmware.bin)
# v0.8.0: esp32-classic war geskippt — data/ sprengte das 256 KB spiffs der
# partitions-4mb.csv. Followup umgesetzt: scripts/fs_exclude_esp32.py strippt
# silence.mp3 (Spotify-only, ~120 KB) NUR fuer env:esp32 aus dem FS-Image
# (PROJECT_DATA_DIR -> gefilterte Staging-Kopie). Damit passt esp32 wieder rein.
"$HOME/.platformio/penv/bin/pio" run -e s3 -e s3-8mb -e c3 -e c6 -e c5 -e esp32 -t buildfs
"$HOME/.platformio/penv/bin/pio" run -e s3 -e s3-8mb -e c3 -e c6 -e c5 -e esp32

# Resolve final version: tag if set, else read the core from version.h.
# Dev FW_VERSION_STRING carries a "+<counter>" suffix (e.g. "0.8.4+1116");
# take only the leading MAJOR.MINOR.PATCH so the manifest version stays clean.
if [ -n "$RELEASE_TAG_STRIPPED" ]; then
  VERSION="$RELEASE_TAG_STRIPPED"
else
  VERSION="$(grep -oE 'FW_VERSION_STRING "[^"]+"' "$ROOT/src/version.h" | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
fi
echo ">>> SixBack release build, version=$VERSION"

# --- 1.5) Size gate: refuse to publish images that don't fit -------------
# v0.7.3 shipped c3/c6 factory-images that exceeded the 0x1C0000 app-slot
# by 8-16 KB. Boot-ROM rejected them with "Image length doesn't fit in
# partition length" -> dead boot-loop on every freshly flashed device.
# Hotfix v0.7.4 enlarged the slot and added this gate so it can't happen
# again silently.
#
# Limits MUST stay in sync with the partition CSVs:
#   partitions-4mb.csv  -> APP_4MB_SYM / FS_4MB_SYM (esp32-classic + c3/c6, A/B-OTA)
#   partitions.csv      -> APP_16MB    / FS_16MB    (s3, OTA)
# c3/c6 zurueck auf A/B-OTA (partitions-4mb.csv) seit mini-silence (30KB) das FS
# unter 256KB drueckt; das alte partitions-4mb-noota.csv wird nicht mehr gebaut.
APP_4MB_SYM=$((0x1D0000))    # 1.900.544 — app0/app1 in partitions-4mb.csv
FS_4MB_SYM=$((0x40000))      #   262.144 — spiffs   in partitions-4mb.csv
APP_16MB=$((0x300000))       # 3.145.728 — app0/app1 in partitions.csv (s3)
FS_16MB=$((0x9E0000))        # 10.354.688 — spiffs in partitions.csv
APP_8MB=$((0x300000))        # 3.145.728 — app0/app1 in partitions-8mb.csv (s3-8mb)
FS_8MB=$((0x1E0000))         #  1.966.080 — spiffs in partitions-8mb.csv

size_errors=0
check_size() {
  local bin="$1" limit="$2" label="$3"
  if [ ! -f "$bin" ]; then
    echo "[size-gate] MISSING: $label ($bin)" >&2
    size_errors=$((size_errors+1)); return 0
  fi
  local sz; sz=$(stat -c '%s' "$bin")
  if [ "$sz" -gt "$limit" ]; then
    printf "[size-gate] OVER: %-12s %8d > %8d (over by %d B) -> %s\n" \
      "$label" "$sz" "$limit" "$((sz - limit))" "$bin" >&2
    size_errors=$((size_errors+1))
  else
    printf "[size-gate] ok:   %-12s %8d / %8d (%d B headroom)\n" \
      "$label" "$sz" "$limit" "$((limit - sz))"
  fi
}

check_size "$PIO_BUILD/esp32/firmware.bin" $APP_4MB_SYM   "esp32 app"
check_size "$PIO_BUILD/esp32/littlefs.bin" $FS_4MB_SYM    "esp32 fs"
check_size "$PIO_BUILD/c3/firmware.bin"    $APP_4MB_SYM   "c3 app"
check_size "$PIO_BUILD/c3/littlefs.bin"    $FS_4MB_SYM    "c3 fs"
check_size "$PIO_BUILD/c6/firmware.bin"    $APP_4MB_SYM   "c6 app"
check_size "$PIO_BUILD/c6/littlefs.bin"    $FS_4MB_SYM    "c6 fs"
# C5: 4-MB-Devkit (DevKitC-1 N4, kein PSRAM) -> gleiches A/B-OTA-Layout wie c3/c6.
check_size "$PIO_BUILD/c5/firmware.bin"    $APP_4MB_SYM   "c5 app"
check_size "$PIO_BUILD/c5/littlefs.bin"    $FS_4MB_SYM    "c5 fs"
check_size "$PIO_BUILD/s3/firmware.bin"    $APP_16MB      "s3 app"
check_size "$PIO_BUILD/s3/littlefs.bin"    $FS_16MB       "s3 fs"
check_size "$PIO_BUILD/s3-8mb/firmware.bin" $APP_8MB      "s3-8mb app"
check_size "$PIO_BUILD/s3-8mb/littlefs.bin" $FS_8MB       "s3-8mb fs"

if [ "$size_errors" -gt 0 ]; then
  echo >&2
  echo "ABORT: $size_errors size violation(s) — refusing to publish." >&2
  echo "  Fix partition table (partitions*.csv) or shrink the build."  >&2
  echo "  Do NOT bypass — devices boot-loop and brick on first flash." >&2
  exit 2
fi

# --- 2) Per-target merge into single factory image -----------------------
ESPTOOL=( "$HOME/.platformio/penv/bin/pio" pkg exec --package "platformio/tool-esptoolpy" -- python -m esptool )

merge_target() {
  local tgt="$1" chip="$2" fsize="$3" spiffs_off="$4" boot_off="$5"
  local src="$PIO_BUILD/$tgt"
  local factory="$OUT/sixback-$tgt-factory.bin"

  echo ">>> Merging $tgt ($chip, $fsize, boot@$boot_off, spiffs@$spiffs_off)"
  "${ESPTOOL[@]}" --chip "$chip" merge_bin \
    -o "$factory" \
    --flash_mode dio --flash_size "$fsize" --flash_freq 80m \
    "$boot_off"   "$src/bootloader.bin" \
    0x8000        "$src/partitions.bin" \
    0x10000       "$src/firmware.bin" \
    "$spiffs_off" "$src/littlefs.bin"

  cp "$src/firmware.bin"  "$OUT/sixback-$tgt-firmware.bin"
  cp "$src/littlefs.bin"  "$OUT/sixback-$tgt-littlefs.bin"
}

# bootloader offset:
#   ESP32 (classic): 0x1000  (Boot-ROM springt dorthin)
#   S3 / C3 / C6 / S2 / C2 / H2: 0x0
#   ESP32-C5 / C61 / P4: 0x2000  (8-KB-Key-Manager-Sektor davor; verifiziert an
#       IDF-Doku + esptool ESP32C5ROM.BOOTLOADER_FLASH_OFFSET + 0xE9 @ 0x2000)
# spiffs offsets must match the corresponding partition table:
#   partitions.csv     (16 MB)     -> spiffs @ 0x610000   (s3)
#   partitions-4mb.csv ( 4 MB A/B) -> spiffs @ 0x3B0000   (esp32-classic + c3/c6)
#       Wer das mal wieder anpasst: hier mitziehen, sonst landet das
#       LittleFS-Image im falschen Flash-Bereich und der Web-Flasher
#       liefert kaputte Factory-Images aus.
merge_target esp32  esp32   4MB  0x3B0000  0x1000   # classic: bootloader@0x1000
merge_target s3     esp32s3 16MB 0x610000  0x0
merge_target s3-8mb esp32s3 8MB  0x610000  0x0      # Seeed XIAO u.a. (Issue #23)
merge_target c3     esp32c3 4MB  0x3B0000  0x0
merge_target c6     esp32c6 4MB  0x3B0000  0x0

# --- C5: eigener Merge (NICHT merge_target) -------------------------------
# Zwei C5-Spezifika, die merge_target nicht abdeckt:
#  (a) bootloader @ 0x2000 (nicht 0x0) — siehe Offset-Kommentar oben.
#  (b) der globale ESPTOOL (platformio/tool-esptoolpy, 4.x) kennt esp32c5 NICHT
#      -> wir brauchen das von der gepinnten pioarduino-Plattform gelieferte
#      esptoolpy-v5.1.2 (enthaelt esptool/targets/esp32c5.py). 5.x akzeptiert
#      die alten Underscore-Flags weiterhin, daher identische Flag-Syntax.
find_c5_esptool() {
  local d
  for d in "$HOME"/.platformio/packages/tool-esptoolpy*; do
    [ -f "$d/esptool/targets/esp32c5.py" ] && { echo "$d"; return 0; }
  done
  return 1
}
C5_ESPTOOL_PKG="$(find_c5_esptool || true)"
if [ -z "$C5_ESPTOOL_PKG" ]; then
  echo "ABORT: kein C5-faehiges esptool gefunden (brauche esptoolpy>=5 mit esp32c5)." >&2
  echo "  -> einmal 'pio run -e c5' laufen lassen; das zieht esptoolpy-v5.1.2."   >&2
  exit 2
fi
echo ">>> Merging c5 (esp32c5, 4MB, boot@0x2000, spiffs@0x3B0000) via $(basename "$C5_ESPTOOL_PKG")"
PYTHONPATH="$C5_ESPTOOL_PKG" "$HOME/.platformio/penv/bin/python" -m esptool --chip esp32c5 merge_bin \
  -o "$OUT/sixback-c5-factory.bin" \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x2000   "$PIO_BUILD/c5/bootloader.bin" \
  0x8000   "$PIO_BUILD/c5/partitions.bin" \
  0x10000  "$PIO_BUILD/c5/firmware.bin" \
  0x3B0000 "$PIO_BUILD/c5/littlefs.bin"
cp "$PIO_BUILD/c5/firmware.bin"  "$OUT/sixback-c5-firmware.bin"
cp "$PIO_BUILD/c5/littlefs.bin"  "$OUT/sixback-c5-littlefs.bin"

# --- 3) Manifest-Writer: atomar + fsync (NFS-Regel) -----------------------
# webflasher/ liegt auf NFS — nackte cat-Redirects koennen dem nachgelagerten
# rsync intermittierend NUL-praefixierte Bloecke zeigen (gleiche Signatur wie
# die gzip-/COMMIT_EDITMSG-Vorfaelle 2026-05-28, vgl. version_bump.py::
# _atomic_write). Daher: tmp + sync + mv, danach JSON-Validierung.
write_manifest() {
  local dst="$1"
  cat > "$dst.tmp"
  sync "$dst.tmp"
  mv "$dst.tmp" "$dst"
  if ! python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$dst"; then
    echo "ABORT: $dst ist kein valides JSON (NFS-Korruption?)" >&2
    exit 2
  fi
}

# --- 3a) Fresh-install manifest (full factory write) ----------------------
# "name" ist hier absichtlich ebenfalls FW_NAME: das factory-Image wird an
# 0x0 geschrieben und bringt Bootloader + Partitionstabelle + App + FS
# komplett mit, ein vorheriges eraseFlash() ist dafuer nicht noetig.
# Folge (bekannt, nicht behoben): laeuft auf dem Board schon SixBack und
# ist das Improv-Fenster offen, greift esp-web-tools' _isSameFirmware und
# es wird OHNE erase geflasht -> die NVS-Partition @0x9000 ueberlebt auf
# Targets, deren factory-Image sie nicht ueberdeckt (S3). Auf C3/C6 liegt
# NVS im Image und wird mitgeschrieben. Der Landing-Page-Text traegt dem
# Rechnung ("ueberschreibt die Firmware" statt "loescht alles").
write_manifest "$OUT/manifest.json" <<EOF
{
  "name": "SixBack",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "sixback-esp32-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "sixback-s3-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C3",
      "parts": [
        { "path": "sixback-c3-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C6",
      "parts": [
        { "path": "sixback-c6-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C5",
      "parts": [
        { "path": "sixback-c5-factory.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

# --- 3b) Update manifest (no erase; firmware + spiffs only, NVS bleibt) --
# Offsets MUESSEN zur Partition-Tabelle des jeweiligen Targets passen:
#   esp32 / s3 / c3 / c6: app/app0 @ 0x10000
#   esp32 / c3 / c6:      spiffs   @ 0x3B0000  (partitions-4mb.csv)
#   s3:                   spiffs   @ 0x610000  (partitions.csv)
#
# ⚠️ "name" MUSS exakt FW_NAME aus src/version.h sein ("SixBack") — sonst
# loescht dieses Manifest das Geraet, statt es zu aktualisieren.
# esp-web-tools entscheidet in install-dialog.ts so:
#
#   if (_isSameFirmware)              _startInstall(false);  // kein erase
#   else if (new_install_prompt_erase) ASK_ERASE;            // fragt nach
#   else                               _startInstall(true);  // eraseFlash()!
#
# und _isSameFirmware ist ein EXAKTER String-Vergleich des per Improv
# gemeldeten Firmware-Namens gegen manifest.name. Der fruehere Wert
# "SixBack (update)" hat nie gematcht -> dritter Zweig -> voller
# eraseFlash() vor dem Schreiben, und da dieses Manifest KEINEN Bootloader
# (@0x0) enthaelt, bliebe ein nicht bootendes Geraet zurueck.
#
# new_install_prompt_erase heisst NICHT "erasen?", sondern "vorher FRAGEN?".
# Deshalb hier true: greift nur, wenn das Improv-Fenster des Geraets schon
# zu ist (dann kennt esp-web-tools den Firmware-Namen nicht). Die Checkbox
# im Dialog ist per Default NICHT angehakt -> auch dieser Pfad erased nicht,
# solange der User es nicht ausdruecklich will.
write_manifest "$OUT/manifest-update.json" <<EOF
{
  "name": "SixBack",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "sixback-esp32-firmware.bin", "offset": 65536    },
        { "path": "sixback-esp32-littlefs.bin", "offset": 3866624  }
      ]
    },
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "sixback-s3-firmware.bin",    "offset": 65536    },
        { "path": "sixback-s3-littlefs.bin",    "offset": 6356992  }
      ]
    },
    {
      "chipFamily": "ESP32-C3",
      "parts": [
        { "path": "sixback-c3-firmware.bin",    "offset": 65536    },
        { "path": "sixback-c3-littlefs.bin",    "offset": 3866624  }
      ]
    },
    {
      "chipFamily": "ESP32-C6",
      "parts": [
        { "path": "sixback-c6-firmware.bin",    "offset": 65536    },
        { "path": "sixback-c6-littlefs.bin",    "offset": 3866624  }
      ]
    },
    {
      "chipFamily": "ESP32-C5",
      "parts": [
        { "path": "sixback-c5-firmware.bin",    "offset": 65536    },
        { "path": "sixback-c5-littlefs.bin",    "offset": 3866624  }
      ]
    }
  ]
}
EOF

# --- 3c) 8-MB-S3-Variante (Seeed XIAO u.a., Issue #23) --------------------
# esp-web-tools waehlt Builds NUR per chipFamily — ein zweiter ESP32-S3-
# Eintrag im Haupt-Manifest waere wirkungslos (der erste gewinnt). Die
# 8-MB-Variante bekommt deshalb ein EIGENES Manifest-Paar + eigenen
# Install-Button auf der Landing-Page. Ein 8-MB-Board am Standard-S3-
# Button wuerde scheitern (16-MB-Factory-Image reicht bis 0xff0000).
# Offsets identisch zum 16-MB-S3 (App-Slots gleich gross, spiffs gleicher
# Offset 0x610000) — nur die spiffs-GROESSE unterscheidet sich.
write_manifest "$OUT/manifest-s3-8mb.json" <<EOF
{
  "name": "SixBack (S3 8MB)",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "sixback-s3-8mb-factory.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

# "name" wie bei 3b exakt FW_NAME — siehe die Begruendung dort. Die
# 8-MB-Unterscheidung traegt der Button auf der Landing-Page, nicht der
# Manifest-Name; esp-web-tools waehlt den Build ohnehin nur per chipFamily.
write_manifest "$OUT/manifest-update-s3-8mb.json" <<EOF
{
  "name": "SixBack",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "sixback-s3-8mb-firmware.bin", "offset": 65536   },
        { "path": "sixback-s3-8mb-littlefs.bin", "offset": 6356992 }
      ]
    }
  ]
}
EOF

# --- 3d) Deploy-Reihenfolge-Guard ------------------------------------------
# Jede in index.html referenzierte Manifest-Datei muss in $OUT existieren —
# verhindert tote Install-Buttons, wenn die Seite vor den Artefakten deployed
# wuerde (Praezedenzfall v0.8.10: Page-only-Fix per rsync).
manifest_missing=0
for m in $(grep -oE 'manifest="[^"]+"' "$OUT/index.html" | cut -d'"' -f2 | sort -u); do
  if [ ! -f "$OUT/$m" ]; then
    echo "ABORT: index.html referenziert $m — fehlt in webflasher/" >&2
    manifest_missing=1
  fi
done
if [ "$manifest_missing" -gt 0 ]; then exit 2; fi

# --- 4) Summary -----------------------------------------------------------
echo
echo "=== Release artefacts (version $VERSION) ==="
ls -lh "$OUT"/*.bin "$OUT"/manifest*.json
echo
echo "Public landing page:  https://sixback.io/"
echo "Deploy command (user triggers manually):"
echo "  rsync -avr webflasher/ 10.0.0.100:/var/www/install/sixback/   # vServer (sixback.io = 31.70.64.234); 10.10.22.1/install.busware.de ist tot"

#!/usr/bin/env bash
# SixBack — schreibt die vier esp-web-tools-Manifeste.
#
#   gen_manifests.sh <version> [outdir]
#
# Aus build_release.sh ausgelagert (2026-07-30), damit die Manifeste OHNE
# einen kompletten 6-Target-Build erzeugt und geprueft werden koennen —
# vorher war der Manifest-Inhalt nur als Nebenprodukt eines 10-Minuten-Builds
# testbar. build_release.sh ruft dieses Skript auf; es bleibt die EINZIGE
# Quelle der Manifeste (keine handgepflegten Kopien in webflasher/).
#
# Zusaetzlich zu den von esp-web-tools gelesenen Feldern (chipFamily, parts,
# name, version, new_install_prompt_erase) traegt jeder Build einen
# "sixback"-Block mit Hardware-Tags. esp-web-tools ignoriert unbekannte
# Felder — sein Manifest/Build-Typ ist ein reines TypeScript-Interface ohne
# Runtime-Validierung (verifiziert gegen esp-web-tools@10.4.0 dist/const.d.ts),
# und die Build-Auswahl der Lib erfolgt weiterhin allein per chipFamily.
# Die Tags wertet ausschliesslich webflasher/index.html aus (Board-Erkennung).
#
# Semantik der Tags — WICHTIG:
#   minFlashSize  Mindest-Flashgroesse, die dieses Image braucht (nicht die
#                 "richtige"!). Ein 4-MB-Layout laeuft auch auf 16 MB Flash,
#                 es nutzt den Rest nur nicht. Ein 16-MB-Layout auf 8 MB
#                 dagegen scheitert mitten im Schreiben. Die Landing-Page
#                 waehlt deshalb: unter allen Builds der erkannten
#                 chipFamily, deren minFlashSize ins vorhandene Flash passt,
#                 gewinnt der mit dem GROESSTEN minFlashSize (= beste
#                 Ausnutzung). Werte exakt wie esptool-js sie meldet
#                 ("4MB"/"8MB"/"16MB", DETECTED_FLASH_SIZES in esploader.js).
#   psram         "required" = das Build ist mit BOARD_HAS_PSRAM kompiliert
#                 (S3-Targets), "none" = ohne. Nur ein HINWEIS, kein
#                 Auswahlkriterium: esptool-js kann embedded PSRAM zwar auf
#                 dem S3 lesen (getPsramCap), auf dem C5 aber NICHT — dort
#                 ist getChipFeatures() hartcodiert ohne PSRAM-Angabe.
#   target        Der PlatformIO-env-Name = Praefix der Artefakt-Dateinamen.
set -euo pipefail

VERSION="${1:?usage: gen_manifests.sh <version> [outdir]}"
OUT="${2:-$(cd "$(dirname "$0")/.." && pwd)/webflasher}"
mkdir -p "$OUT"

# Manifest-Writer: atomar + fsync (NFS-Regel).
# webflasher/ liegt auf NFS — nackte cat-Redirects koennen dem nachgelagerten
# rsync intermittierend NUL-praefixierte Bloecke zeigen (gleiche Signatur wie
# die gzip-/COMMIT_EDITMSG-Vorfaelle 2026-05-28, vgl. version_bump.py::
# _atomic_write). Daher: tmp + sync + mv, danach JSON-Validierung.
#
# ⚠️ mktemp NICHT verwenden — es legt mit Mode 0600 an, os.replace/mv behaelt
# das bei, rsync -a traegt es auf den Server und Apache antwortet dann mit
# 403 (AH00132, Vorfall 2026-07-29). Der cat-Redirect hier erbt die umask.
write_manifest() {
  local dst="$1"
  cat > "$dst.tmp"
  sync "$dst.tmp"
  mv "$dst.tmp" "$dst"
  if ! python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$dst"; then
    echo "ABORT: $dst ist kein valides JSON (NFS-Korruption?)" >&2
    exit 2
  fi
  chmod 644 "$dst"
}

# --- a) Fresh-install manifest (full factory write) -----------------------
# "name" ist hier absichtlich ebenfalls FW_NAME: das factory-Image wird an
# 0x0 geschrieben und bringt Bootloader + Partitionstabelle + App + FS
# komplett mit, ein vorheriges eraseFlash() ist dafuer nicht noetig.
# Folge (bekannt, nicht behoben): laeuft auf dem Board schon SixBack und
# ist das Improv-Fenster offen, greift esp-web-tools' _isSameFirmware und
# es wird OHNE erase geflasht -> die NVS-Partition @0x9000 ueberlebt auf
# Targets, deren factory-Image sie nicht ueberdeckt (S3). Auf C3/C6 liegt
# NVS im Image und wird mitgeschrieben.
write_manifest "$OUT/manifest.json" <<EOF
{
  "name": "SixBack",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "sixback": { "kind": "factory", "variant": "standard" },
  "builds": [
    {
      "chipFamily": "ESP32",
      "sixback": { "target": "esp32", "minFlashSize": "4MB", "psram": "none" },
      "parts": [
        { "path": "sixback-esp32-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-S3",
      "sixback": { "target": "s3", "minFlashSize": "16MB", "psram": "required" },
      "parts": [
        { "path": "sixback-s3-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C3",
      "sixback": { "target": "c3", "minFlashSize": "4MB", "psram": "none" },
      "parts": [
        { "path": "sixback-c3-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C6",
      "sixback": { "target": "c6", "minFlashSize": "4MB", "psram": "none" },
      "parts": [
        { "path": "sixback-c6-factory.bin", "offset": 0 }
      ]
    },
    {
      "chipFamily": "ESP32-C5",
      "sixback": { "target": "c5", "minFlashSize": "4MB", "psram": "none" },
      "parts": [
        { "path": "sixback-c5-factory.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

# --- b) Update manifest (no erase; firmware + spiffs only, NVS bleibt) ----
# Offsets MUESSEN zur Partition-Tabelle des jeweiligen Targets passen:
#   esp32 / s3 / c3 / c6 / c5: app/app0 @ 0x10000
#   esp32 / c3 / c6 / c5:      spiffs   @ 0x3B0000  (partitions-4mb.csv)
#   s3:                        spiffs   @ 0x610000  (partitions.csv)
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
# (@0x0) enthaelt, blieb ein nicht bootendes Geraet zurueck (hardware-belegt
# 2026-07-29: Endlos-Loop "invalid header: 0xffffffff").
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
  "sixback": { "kind": "update", "variant": "standard" },
  "builds": [
    {
      "chipFamily": "ESP32",
      "sixback": { "target": "esp32", "minFlashSize": "4MB", "psram": "none" },
      "parts": [
        { "path": "sixback-esp32-firmware.bin", "offset": 65536    },
        { "path": "sixback-esp32-littlefs.bin", "offset": 3866624  }
      ]
    },
    {
      "chipFamily": "ESP32-S3",
      "sixback": { "target": "s3", "minFlashSize": "16MB", "psram": "required" },
      "parts": [
        { "path": "sixback-s3-firmware.bin",    "offset": 65536    },
        { "path": "sixback-s3-littlefs.bin",    "offset": 6356992  }
      ]
    },
    {
      "chipFamily": "ESP32-C3",
      "sixback": { "target": "c3", "minFlashSize": "4MB", "psram": "none" },
      "parts": [
        { "path": "sixback-c3-firmware.bin",    "offset": 65536    },
        { "path": "sixback-c3-littlefs.bin",    "offset": 3866624  }
      ]
    },
    {
      "chipFamily": "ESP32-C6",
      "sixback": { "target": "c6", "minFlashSize": "4MB", "psram": "none" },
      "parts": [
        { "path": "sixback-c6-firmware.bin",    "offset": 65536    },
        { "path": "sixback-c6-littlefs.bin",    "offset": 3866624  }
      ]
    },
    {
      "chipFamily": "ESP32-C5",
      "sixback": { "target": "c5", "minFlashSize": "4MB", "psram": "none" },
      "parts": [
        { "path": "sixback-c5-firmware.bin",    "offset": 65536    },
        { "path": "sixback-c5-littlefs.bin",    "offset": 3866624  }
      ]
    }
  ]
}
EOF

# --- c) 8-MB-S3-Variante (Seeed XIAO u.a., Issue #23) ---------------------
# esp-web-tools waehlt Builds NUR per chipFamily — ein zweiter ESP32-S3-
# Eintrag im Haupt-Manifest waere wirkungslos (der erste gewinnt). Die
# 8-MB-Variante bekommt deshalb ein EIGENES Manifest-Paar + eigenen
# Install-Button auf der Landing-Page. Ein 8-MB-Board am Standard-S3-
# Button wuerde scheitern (16-MB-Factory-Image reicht bis 0xff0000).
# Genau diese Verwechslung waehlt seit 2026-07-30 die Board-Erkennung der
# Landing-Page anhand der minFlashSize-Tags ab.
# Offsets identisch zum 16-MB-S3 (App-Slots gleich gross, spiffs gleicher
# Offset 0x610000) — nur die spiffs-GROESSE unterscheidet sich.
write_manifest "$OUT/manifest-s3-8mb.json" <<EOF
{
  "name": "SixBack (S3 8MB)",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "sixback": { "kind": "factory", "variant": "s3-8mb" },
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "sixback": { "target": "s3-8mb", "minFlashSize": "8MB", "psram": "required" },
      "parts": [
        { "path": "sixback-s3-8mb-factory.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

# "name" wie bei b) exakt FW_NAME — siehe die Begruendung dort. Die
# 8-MB-Unterscheidung traegt der Button auf der Landing-Page, nicht der
# Manifest-Name; esp-web-tools waehlt den Build ohnehin nur per chipFamily.
write_manifest "$OUT/manifest-update-s3-8mb.json" <<EOF
{
  "name": "SixBack",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "sixback": { "kind": "update", "variant": "s3-8mb" },
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "sixback": { "target": "s3-8mb", "minFlashSize": "8MB", "psram": "required" },
      "parts": [
        { "path": "sixback-s3-8mb-firmware.bin", "offset": 65536   },
        { "path": "sixback-s3-8mb-littlefs.bin", "offset": 6356992 }
      ]
    }
  ]
}
EOF

# --- d) 16-MB-PSRAM-C5-Variante (ESP32-C5-WROOM-1-N16R8 u.a.) -------------
# Wie bei c): esp-web-tools waehlt nur per chipFamily, ein zweiter ESP32-C5-
# Eintrag im Haupt-Manifest waere wirkungslos -> eigenes Manifest-Paar +
# eigene Buttons. Das Standard-c5-Image (4-MB-Layout) laeuft auf 16-MB-Boards
# zwar, laesst aber 12 MB Flash + 8 MB PSRAM ungenutzt; umgekehrt scheitert
# das 16-MB-Image auf einem N4 mitten im Schreiben — genau die Verwechslung,
# die die Board-Erkennung ueber minFlashSize abwaehlt.
# psram "required": Build erwartet in-package PSRAM (N16R8-Klasse); die
# Erkennung warnt auf einem C5 ohne PSRAM (eFuse PSRAM_CAP=0).
write_manifest "$OUT/manifest-c5-16mb.json" <<EOF
{
  "name": "SixBack (C5 16MB)",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "sixback": { "kind": "factory", "variant": "c5-16mb" },
  "builds": [
    {
      "chipFamily": "ESP32-C5",
      "sixback": { "target": "c5-16mb", "minFlashSize": "16MB", "psram": "required" },
      "parts": [
        { "path": "sixback-c5-16mb-factory.bin", "offset": 0 }
      ]
    }
  ]
}
EOF

# "name" exakt FW_NAME — Begruendung bei b). Offsets: app @0x10000,
# spiffs @0x610000 (partitions.csv, identisch s3).
write_manifest "$OUT/manifest-update-c5-16mb.json" <<EOF
{
  "name": "SixBack",
  "version": "$VERSION",
  "funding_url": "https://paypal.me/busware",
  "new_install_prompt_erase": true,
  "sixback": { "kind": "update", "variant": "c5-16mb" },
  "builds": [
    {
      "chipFamily": "ESP32-C5",
      "sixback": { "target": "c5-16mb", "minFlashSize": "16MB", "psram": "required" },
      "parts": [
        { "path": "sixback-c5-16mb-firmware.bin", "offset": 65536   },
        { "path": "sixback-c5-16mb-littlefs.bin", "offset": 6356992 }
      ]
    }
  ]
}
EOF

echo ">>> manifests written to $OUT (version=$VERSION)"

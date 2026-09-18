# esptool-js-Harness — "Identify my board" headless nachfahren

Fährt **esptool-js** (dieselbe Version, die `webflasher/board-detect.js` per CDN lädt)
gegen ein Board am Lab-Host und zeigt, was das Detect-Widget im Browser bekäme —
ohne Board am Desktop, ohne Web Serial, ohne Chrome.

Gebaut am 2026-08-17, um einen Feldbericht nachzumessen: „C5 N16R8 wird als 4-MB-Version
erkannt, das Gerät meldet aber 16 MB".

## Benutzen

```sh
npm install                     # serialport + esptool-js
node identify.js /dev/serial/by-id/usb-…-if00
```

Optionen:

| Flag | Wirkung |
|---|---|
| `--lib=esptool-js-061/bundle.js` | andere Lib-Version vergleichen (`npm i esptool-js-061@npm:esptool-js@0.6.1`) |
| `--boards-url=…` | andere Flasher-Seite als `https://sixback.io/boards.html` |
| `--spi-attach` | `flashSpiAttach(0)` vor dem Lesen (Nachbau eines esptool-py-Schritts) |

Ausgegeben werden: erkannter Chip, die **rohe flash_id** samt Kapazitätsbyte, ob dieses
Byte in `DETECTED_FLASH_SIZES` steht, der Rückgabewert von `detectFlashSize()`, PSRAM
(S3 über `getPsramCap`, C5 über die eFuse `PSRAM_CAP`) — und welchen Install-Button die
Seite mit diesen Werten als `✓ use this one` markieren würde.

## Warum die drei Dateien so aussehen

- **`identify.js`** importiert bewusst `esptool-js/bundle.js`, nicht `lib/index.js`:
  die gebündelte Datei ist bitgleich mit der unpkg-Auslieferung, die der Browser lädt,
  und `lib/*.js` benutzt extension-lose Imports, die Node-ESM nicht auflöst.
  Anders als die Seite hält der Harness das Lib-Terminal **nicht** stumm — genau dort
  steht die Zeile „Could not auto-detect Flash size. defaulting to 4MB", die im Browser
  verloren geht.
- **`node-web-serial.js`** bildet die sechs Web-Serial-Aufrufe nach, die esptool-js
  benutzt (`open/close/getInfo/readable/writable/setSignals`). `getInfo()` liefert die
  echten VID/PID, weil die Lib darüber die Reset-Strategie wählt (PID `0x1001` =
  nativer USB-Serial-JTAG).
- **`tiocm-helper.py`** existiert wegen eines node-serialport-Details: dessen `set()`
  ruft unbedingt `TIOCSBRK`/`TIOCCBRK`, bevor es zu `TIOCMSET` kommt. `cdc_acm` kann
  kein break → `EOPNOTSUPP` → DTR/RTS werden nie gesetzt → keine Reset-Sequenz. Der
  Helfer macht nur die beiden ioctls, ohne termios anzufassen. Der Adapter versucht
  erst den direkten Weg und schaltet nur bei Fehler um.

## Befund vom 2026-08-17 — Ursache: falsche SPI-Registerbasis in esptool-js

esptool-js benutzt für die neuere RISC-V-Familie den SPI-Controller des C3
(`SPI_REG_BASE = 0x60002000`). esptool-py setzt für **C5, C6, C61, H2** dagegen
`0x60003000`. Dadurch laufen in der JS-Lib alle SPI-Flash-Kommandos auf den falschen
Controller: `readFlashId()` liefert 0, `detectFlashSize()` fällt still auf `"4MB"`
zurück — ununterscheidbar von einem echten 4-MB-Fund.

| Board | esptool-py | esptool-js, Basis `0x60002000` | esptool-js, Basis `0x60003000` |
|---|---|---|---|
| C5 N16R8 (16 MB) | `0x184046` = 16MB | `0x000000` → Default 4MB | `0x184046` = 16MB ✓ |
| C6FH4 (4 MB) | `20`/`4016` = 4MB | `0x000000` → Default 4MB | `0x164020` = 4MB ✓ |
| H2 (4 MB) | `c8`/`4016` = 4MB | — | `0x1640c8` = 4MB ✓ |

Weder ein Bump auf esptool-js 0.6.1 noch ein vorgeschaltetes `flashSpiAttach(0)` ändern
etwas; Chip-Erkennung, MAC und der PSRAM-eFuse-Pfad funktionieren durchgehend.

Folge auf der Flasher-Seite vor dem Fix: auf einem echten 16-MB-C5 wurde das 4-MB-Paar
als `✓ use this one` markiert und das `c5-16mb`-Paar mit „needs 16MB, board has 4MB"
abgelehnt. `webflasher/board-detect.js` korrigiert die Basis seit 2026-08-17 selbst und
liest die flash_id roh, statt dem stillen Default zu vertrauen.

`--no-c5-fix` stellt den kaputten Ausgangszustand wieder her, um den Bug vorzuführen.

## Upstream-Verifikation von esptool-js#253 (2026-08-18)

Der PR wurde mit diesem Harness am Blech nachgemessen: PR-Branch klonen, `npm run build`,
das entstandene `bundle.js` per Symlink als `node_modules/esptool-js-pr253` einhaengen und
mit `--no-c5-fix` fahren (Harness-eigener Patch AUS, gemessen wird die Lib allein).

| Board | esptool-py 5.1.2 | esptool-js 0.6.1 | PR #253 |
|---|---|---|---|
| C5 N16R8 (16 MB) | `0x184046` | `0x000000` -> still `"4MB"` | `0x184046` -> 16MB |
| C6FH4 (4 MB) | `0x164020` | `0x000000` -> still `"4MB"` | `0x164020` -> 4MB |
| H2 (4 MB) | `0x1640c8` | `0x000000` -> still `"4MB"` | `0x1640c8` -> 4MB |

`probe-undefined.js` prueft den zweiten Teil des PR: es dreht nach `main()` die SPI-Basis
absichtlich auf `0x60002000` zurueck, damit der Flash wirklich nicht antwortet, und zeigt
den Rueckgabewert von `detectFlashSize()` — `"4MB"` (String) auf 0.6.1, `undefined` mit
dem PR.

Dabei fiel ein aelterer Fehler auf, der nicht zu diesem PR gehoert und getrennt gemeldet
ist (esptool-js#254): `writeFlash({ flashSize: "detect" })` wirft immer
`File N doesn't fit in the available flash`, weil `flashSizeBytes("detect")` `-1` liefert
und die Groessenpruefung vor der Erkennung laeuft. Ohne Board reproduzierbar.

**Nach jedem Harness-Lauf einen esptool-Hard-Reset hinterherschieben.** Der `finally`-Block
holt C5 (CH343) und C6 (USB-JTAG) nicht zuverlaessig aus dem Stub zurueck — beide standen
danach still im Bootloader. Sauber zurueck kamen sie erst mit
`python3 -m esptool --port <dev> --after hard_reset flash_id`, was zugleich die
Ground-truth-Messung ist.

## Re-Test gegen den finalen PR-Stand `ca1604f2` (2026-08-19)

Nach zwei Review-Runden hat der Maintainer nachgelegt: `cba2dc10` loest die Flash-Groesse
in `writeFlash` auf, bevor die Groessenpruefung laeuft (das ist der Fix fuer #254), und
`ca1604f2` entfernt den nun unerreichbaren `"detect"`-Zweig aus
`_updateImageFlashParams`, dokumentiert das per JSDoc und legt einen CHANGELOG an, der
`detectFlashSize(): string|undefined` als Breaking Change ausweist.

Gleiches Rezept wie am 18.08. (Branch bauen, Bundle per Symlink einhaengen,
`--no-c5-fix`). Ground truth mit esptool **5.1.2** aus dem PlatformIO-penv
(`/root/.platformio/penv/bin/python -m esptool`) — das global installierte
esptool.py 4.7.0 kennt den C5 nicht („Unexpected chip ID value 23").

| Board | esptool-py 5.1.2 | esptool-js 0.6.1 | PR `ca1604f2` |
|---|---|---|---|
| C5 N16R8 (16 MB) | Mfr 46 / Dev 4018 = 16MB | `0x000000` -> still `"4MB"` | `0x184046` -> 16MB |
| C6FH4 (4 MB) | Mfr 20 / Dev 4016 = 4MB | `0x000000` -> still `"4MB"` | `0x164020` -> 4MB |
| H2 (4 MB) | Mfr c8 / Dev 4016 = 4MB | `0x000000` -> still `"4MB"` | `0x1640c8` -> 4MB |

Seiten-Verdikt am C5 kippt weiterhin korrekt auf `c5-16mb`. Im gebauten (minifizierten)
Bundle tragen genau zwei Klassen `SPI_REG_BASE=1610625024` (= `0x60003000`), C5/C61 erben
-> alle vier Targets gedeckt.

`probe-254.js` (neu, **ohne Hardware**) faehrt `writeFlash({flashSize:"detect"})` ueber
`Object.create(ESPLoader.prototype)` mit gestubbtem `detectFlashSize()` und gestubbtem
`_updateImageFlashParams`, das einen Sentinel wirft, sobald die Groessenpruefung passiert
ist:

| Fall | 0.6.1 | PR `ca1604f2` |
|---|---|---|
| detect -> `"4MB"`, 4 KB @0x0 | `File 1 doesn't fit` | Pruefung passiert, `_updateImageFlashParams` sieht `"4MB"` |
| detect -> `"16MB"`, 4 KB @0x0 | `File 1 doesn't fit` | Pruefung passiert, sieht `"16MB"` |
| detect -> `"4MB"`, 8 MB @0x0 | `File 1 doesn't fit` | `File 1 doesn't fit` (korrekt, echt zu gross) |
| detect -> `undefined` | `File 1 doesn't fit` | `Could not auto-detect Flash size…` |

Der dritte Fall ist der wichtige Gegencheck: die Pruefung wurde nicht entschaerft, nur
gegen den richtigen Wert gefahren. Am Blech bestaetigt `probe-undefined.js` denselben
Stand: `detectFlashSize()` liefert `undefined` (0.6.1: `"4MB"`, typeof string), und
`writeFlash` wirft die verstaendliche Meldung statt der irrefuehrenden.

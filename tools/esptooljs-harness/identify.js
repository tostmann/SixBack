// SixBack — "Identify my board" headless nachfahren.
//
// Faehrt esptool-js 0.6.0 (dieselbe Version, die webflasher/board-detect.js per
// CDN laedt) gegen ein Board am Lab-Host und protokolliert, was die Lib dem
// Detect-Widget liefern WUERDE. Zweck: Feldberichte wie "meine 16-MB-Karte wird
// als 4MB erkannt" reproduzierbar nachmessen, ohne Board am Desktop.
//
// Zwei Dinge, die die Seite selbst nicht zeigt und hier bewusst sichtbar sind:
//  1. die ROHE flash_id — detectFlashSize() faellt bei einem Kapazitaetsbyte,
//     das nicht in DETECTED_FLASH_SIZES steht, still auf "4MB" zurueck; der
//     Rueckgabewert ist dann nicht von echten 4 MB unterscheidbar.
//  2. die Log-Zeilen der Lib — board-detect.js gibt ihr ein stummes Terminal,
//     genau die "Could not auto-detect Flash size"-Meldung geht dort verloren.
//
// Aufruf:  node identify.js /dev/serial/by-id/usb-...-if00 [--boards-url URL]
// Bewusst das BUNDLE, nicht lib/index.js: die gebuendelte Datei ist bitgenau die,
// die board-detect.js per CDN laedt (sha256 der npm-Kopie == unpkg-Auslieferung,
// 7c361337d5bba7271cb0d9741f165a3b87137ff9284c13f112a6e197c48cd0da). Damit laeuft
// hier derselbe Code wie in Chrome — und lib/*.js hat extension-lose Imports, die
// Node-ESM ohnehin nicht aufloest.
import { NodeWebSerialPort, findPort } from './node-web-serial.js';

const args = process.argv.slice(2);
const devArg = args.find((a) => !a.startsWith('--'));
const opt = (name, dflt) =>
  (args.find((a) => a.startsWith(`--${name}=`)) || `=${dflt}`).split('=').slice(1).join('=');
const boardsUrl = opt('boards-url', 'https://sixback.io/boards.html');
// --lib erlaubt den Versionsvergleich (z.B. --lib=esptool-js-061/bundle.js), ohne
// den CDN-Pin der Seite anzufassen.
const libSpec = opt('lib', 'esptool-js/bundle.js');
if (!devArg) {
  console.error('usage: node identify.js <serial-device> [--lib=esptool-js/bundle.js]'
    + ' [--boards-url=https://…/boards.html]');
  process.exit(2);
}
const { ESPLoader, Transport, UsbJtagSerialReset } = await import(libSpec);
console.error(`Lib: ${libSpec}`);

const libLog = [];
const terminal = {
  clean() {},
  writeLine(s) { libLog.push(s); console.error('  [esptool-js] ' + s); },
  write(s) { libLog.push(s); },
};

const mib = (s) => { const m = /^(\d+)MB$/.exec(s || ''); return m ? +m[1] : 0; };

/** Button-Liste aus der echten Seite ziehen statt hartkodieren (wie die Seite aus dem DOM). */
async function manifestUrls(url) {
  const html = await (await fetch(url)).text();
  const base = new URL(url);
  const urls = [...html.matchAll(/<esp-web-install-button[^>]*\smanifest="([^"]+)"/g)]
    .map((m) => new URL(m[1], base).href);
  return [...new Set(urls)];
}

/** Auswahlregel aus board-detect.js: pro kind gewinnt das groesste noch passende Layout. */
async function verdicts(det, urls) {
  const choices = [];
  for (const url of urls) {
    let m = {};
    try { m = await (await fetch(url)).json(); } catch { /* Button bleibt unbewertet */ }
    const build = (m.builds || []).find((b) => b.chipFamily === det.chip);
    choices.push({
      url, build,
      tags: (build && build.sixback) || {},
      kind: (m.sixback && m.sixback.kind) || url,
      variant: (m.sixback && m.sixback.variant) || '?',
    });
  }
  const best = new Map();
  for (const c of choices) {
    const need = mib(c.tags.minFlashSize);
    if (!c.build || !need || need > det.flashMib) continue;
    const cur = best.get(c.kind);
    if (!cur || need > mib(cur.tags.minFlashSize)) best.set(c.kind, c);
  }
  return choices.map((c) => {
    const need = mib(c.tags.minFlashSize);
    let verdict;
    if (!c.build) verdict = `not built for ${det.chip}`;
    else if (need > det.flashMib) verdict = `needs ${c.tags.minFlashSize}, board has ${det.flash}`;
    else if (best.get(c.kind) === c) verdict = '✓ use this one';
    else verdict = `${c.tags.minFlashSize} layout — smaller than your flash`;
    return { ...c, verdict };
  });
}

const { path, info } = await findPort(devArg);
console.error(`Port ${path}  VID:PID ${info.usbVendorId.toString(16).padStart(4, '0')}:`
  + `${info.usbProductId.toString(16).padStart(4, '0')}`);

const device = new NodeWebSerialPort(path, info);
const transport = new Transport(device, false);
const loader = new ESPLoader({ transport, baudrate: 115200, terminal });

let out = {};
try {
  await loader.main();

  const chip = loader.chip.CHIP_NAME;

  // Experiment: esptool-py schiebt vor dem Lesen ein SPI-Attach ein ("Enabling
  // default SPI flash mode") und liest danach korrekt. --spi-attach probiert, ob
  // derselbe Schritt der JS-Lib zu einer brauchbaren flash_id verhilft.
  if (args.includes('--spi-attach')) {
    await loader.flashSpiAttach(0);
    console.error('  [harness] flashSpiAttach(0) ausgefuehrt');
  }

  // Welchen SPI-Controller adressiert die Lib? esptool-py setzt fuer den C5
  // explizit SPI_REG_BASE = 0x60003000 und ueberschreibt damit den vom C6
  // geerbten Wert 0x60002000. Fehlt diese Ueberschreibung, laufen alle
  // SPI-Flash-Kommandos auf den falschen Controller.
  console.error(`  [harness] chip.SPI_REG_BASE = 0x${(loader.chip.SPI_REG_BASE >>> 0).toString(16)}`);
  const spiBase = opt('spi-base', '');
  if (spiBase) {
    loader.chip.SPI_REG_BASE = Number(spiBase);
    console.error(`  [harness] SPI_REG_BASE ueberschrieben auf ${spiBase}`);
  } else if (!args.includes('--no-c5-fix')
             && ['ESP32-C5', 'ESP32-C6', 'ESP32-C61', 'ESP32-H2'].includes(chip)
             && loader.chip.SPI_REG_BASE === 0x60002000) {
    // Derselbe Fix, den board-detect.js jetzt ausliefert: esptool-py setzt fuer
    // C5/C6/C61/H2 0x60003000, esptool-js benutzt dort den C3-Wert 0x60002000.
    // --no-c5-fix stellt den kaputten Ausgangszustand wieder her.
    loader.chip.SPI_REG_BASE = 0x60003000;
    console.error('  [harness] SPI-Base-Fix aktiv: SPI_REG_BASE -> 0x60003000');
  }

  // ROH zuerst: dieselbe Quelle, aus der detectFlashSize() sein Byte zieht.
  const flashId = await loader.readFlashId();
  const manufacturer = flashId & 0xff;
  const memType = (flashId >> 8) & 0xff;
  const capacity = (flashId >> 16) & 0xff;
  const inTable = loader.DETECTED_FLASH_SIZES[capacity];

  const flash = await loader.detectFlashSize();

  // PSRAM: S3 kann getPsramCap, beim C5 liest board-detect.js die eFuse direkt
  // (PSRAM_CAP, Bits 21:19 von EFUSE_RD_MAC_SYS2_REG = EFUSE_BASE + 0x4C).
  let psram = null, psramRaw = null;
  if (typeof loader.chip.getPsramCap === 'function') {
    const feats = await loader.chip.getChipFeatures(loader);
    psram = feats.find((f) => /PSRAM/i.test(f)) || false;
  } else if (chip === 'ESP32-C5' && loader.chip.EFUSE_BASE) {
    const w = await loader.readReg(loader.chip.EFUSE_BASE + 0x4c);
    const cap = (w >>> 19) & 0x7;
    psramRaw = '0x' + (w >>> 0).toString(16).padStart(8, '0');
    psram = cap === 0 ? false : `in-package PSRAM${cap === 2 ? ' 8MB' : ''} (eFuse PSRAM_CAP=${cap})`;
  }

  out = {
    chip,
    flash_id: '0x' + flashId.toString(16).padStart(6, '0'),
    manufacturer: '0x' + manufacturer.toString(16).padStart(2, '0'),
    memory_type: '0x' + memType.toString(16).padStart(2, '0'),
    capacity_byte: '0x' + capacity.toString(16).padStart(2, '0'),
    capacity_in_lookup_table: inTable ?? null,
    detectFlashSize_returned: flash,
    silent_4mb_fallback: !inTable,
    psram, psram_efuse_word: psramRaw,
  };

  console.log('\n=== esptool-js sieht ===');
  console.log(JSON.stringify(out, null, 2));
  if (!inTable) {
    console.log('\n⚠ Kapazitaetsbyte steht NICHT in DETECTED_FLASH_SIZES —'
      + ' der "4MB" oben ist der stille Default, keine Messung.');
  }

  const urls = await manifestUrls(boardsUrl);
  const rows = await verdicts(
    { chip, flash, flashMib: mib(flash), psram }, urls);
  console.log('\n=== Welchen Knopf wuerde die Seite markieren ===');
  for (const r of rows) {
    console.log(`  ${r.verdict.padEnd(42)} ${r.kind}/${r.variant}  ${r.url.split('/').pop()}`);
  }
} catch (e) {
  console.error('FEHLGESCHLAGEN: ' + (e?.message ?? e));
  process.exitCode = 1;
} finally {
  // Board nicht im Download-Stub stehen lassen. Auf nativem USB (PID 0x1001)
  // pulst after('hard_reset') nur RTS und wirkt nicht — dann die JTAG-Sequenz,
  // genau wie board-detect.js es macht.
  try {
    if (info.usbProductId === 0x1001) await new UsbJtagSerialReset(transport).reset();
    else await loader.after('hard_reset');
  } catch { /* best effort */ }
  try { await transport.disconnect(); } catch { /* best effort */ }
  try { await device.close(); } catch { /* best effort */ }
}

// Hardware-freier Nachweis fuer esptool-js#254: writeFlash({flashSize:"detect"})
// Vorher: wirft immer "File N doesn't fit in the available flash", weil
// flashSizeBytes("detect") == -1 und die Groessenpruefung VOR der Erkennung lief.
// Erwartung mit PR #253 (cba2dc10): Groesse wird zuerst aufgeloest, die Pruefung
// laeuft gegen den erkannten Wert, und _updateImageFlashParams bekommt den
// AUFGELOESTEN String (nicht mehr "detect").
const libSpec = process.argv[2];
const { ESPLoader } = await import(libSpec);

async function run(label, detected, fileLen, address) {
  const l = Object.create(ESPLoader.prototype);
  l.debug = () => {}; l.info = () => {}; l.error = () => {};
  l.transport = { trace() {} };
  l.IS_STUB = false;
  l.detectFlashSize = async () => detected;
  let sawFlashSize = '<nie gerufen>';
  l._updateImageFlashParams = async (_img, _addr, _m, _f, flashSize) => {
    sawFlashSize = JSON.stringify(flashSize);
    throw new Error('SENTINEL_REACHED_UPDATE_PARAMS');
  };
  let verdict;
  try {
    await l.writeFlash({
      fileArray: [{ data: new Uint8Array(fileLen), address }],
      flashSize: 'detect', flashMode: 'keep', flashFreq: 'keep',
      eraseAll: false, compress: true,
    });
    verdict = 'kein Fehler';
  } catch (e) {
    verdict = e.message === 'SENTINEL_REACHED_UPDATE_PARAMS'
      ? 'Groessenpruefung PASSIERT (bis _updateImageFlashParams gekommen)'
      : e.constructor.name + ': ' + e.message;
  }
  console.log(`  ${label.padEnd(46)} -> ${verdict}`);
  console.log(`  ${''.padEnd(46)}    _updateImageFlashParams sah flashSize=${sawFlashSize}`);
}

console.log('lib =', libSpec);
await run('detect->"4MB", 4 KB @0x0 (muss durchgehen)',      '4MB',     4096, 0);
await run('detect->"16MB", 4 KB @0x0 (muss durchgehen)',     '16MB',    4096, 0);
await run('detect->"4MB", 8 MB @0x0 (muss ECHT nicht passen)','4MB', 8*1024*1024, 0);
await run('detect->undefined (Flash antwortet nicht)',        undefined, 4096, 0);

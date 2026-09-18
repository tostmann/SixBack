// Prueft den ZWEITEN Teil von esptool-js#253 am Blech: liefert detectFlashSize()
// bei unlesbarem Flash jetzt `undefined` statt des stillen "4MB"?
// Trick: nach dem Verbinden die SPI-Basis absichtlich auf den kaputten C3-Wert
// zurueckdrehen -> der Flash-Chip antwortet nicht -> genau der Feldfehlerfall.
import { NodeWebSerialPort, findPort } from './node-web-serial.js';
const [dev, libSpec] = process.argv.slice(2);
const { ESPLoader, Transport, UsbJtagSerialReset } = await import(libSpec);
const terminal = { clean() {}, writeLine(s) { console.error('  [esptool-js] ' + s); }, write() {} };
const { path, info } = await findPort(dev);
const transport = new Transport(new NodeWebSerialPort(path, info), false);
const loader = new ESPLoader({ transport, baudrate: 115200, terminal, debugLogging: false });
await loader.main();
console.log('chip                :', loader.chip.CHIP_NAME);
console.log('SPI_REG_BASE (Lib)  : 0x' + loader.chip.SPI_REG_BASE.toString(16));
loader.chip.SPI_REG_BASE = 0x60002000;      // absichtlich kaputt
console.log('SPI_REG_BASE forciert: 0x60002000  (Flash antwortet jetzt nicht)');
const id = await loader.readFlashId();
console.log('readFlashId()       : 0x' + id.toString(16).padStart(6, '0'));
const size = await loader.detectFlashSize();
console.log('detectFlashSize()   :', JSON.stringify(size), '  typeof =', typeof size);
try {
  await loader.writeFlash({ fileArray: [], flashSize: 'detect', eraseAll: false, compress: true });
  console.log('writeFlash(detect)  : KEIN Fehler geworfen');
} catch (e) {
  console.log('writeFlash(detect)  : wirft ->', e.constructor.name + ': ' + e.message);
}
// Board nicht im Stub stehen lassen — gleiche Reihenfolge wie identify.js.
// ACHTUNG: bei C5 (CH343) und C6 (USB-JTAG) reicht das in der Praxis NICHT immer;
// kommt das Board danach nicht ins Netz zurueck, hilft
//   python3 -m esptool --port <dev> --after hard_reset flash_id
// das ist zugleich der Ground-truth-Lauf gegen esptool-py.
try {
  if (info.usbProductId === 0x1001) await new UsbJtagSerialReset(transport).reset();
  else await loader.after('hard_reset');
} catch { /* best effort */ }
try { await transport.disconnect(); } catch { /* best effort */ }

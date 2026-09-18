// Minimaler Web-Serial-Adapter fuer Node, damit esptool-js UNVERAENDERT laufen kann.
//
// esptool-js spricht ausschliesslich die Web-Serial-API des Browsers an. Laut
// lib/webserial.js benutzt es genau sechs Dinge am Port-Objekt:
//   open() · close() · getInfo() · readable · writable · setSignals()
// Dieser Wrapper bildet die auf node-serialport ab. Damit laeuft im Harness
// exakt derselbe Lib-Code wie in Chrome — kein Nachbau der Leseschicht.
import { SerialPort } from 'serialport';

export class NodeWebSerialPort {
  /**
   * @param {string} path   z.B. /dev/ttyACM9
   * @param {{usbVendorId:number, usbProductId:number}} info  VID/PID wie sie der
   *   Browser meldet. WICHTIG: esptool-js waehlt die Reset-Strategie ueber
   *   getInfo().usbProductId (0x1001 = nativer USB-Serial-JTAG), also muessen
   *   hier die echten Werte stehen, sonst weicht das Verhalten vom Browser ab.
   */
  constructor(path, info) {
    this.path = path;
    this._info = info;
    this.readable = null;
    this.writable = null;
  }

  getInfo() {
    return this._info;
  }

  async open(options = {}) {
    const baudRate = options.baudRate ?? 115200;
    this._port = new SerialPort({ path: this.path, baudRate, autoOpen: false });
    await new Promise((res, rej) => this._port.open((e) => (e ? rej(e) : res())));

    this.readable = new ReadableStream({
      start: (controller) => {
        this._onData = (chunk) => {
          try { controller.enqueue(new Uint8Array(chunk)); } catch { /* Stream zu */ }
        };
        this._port.on('data', this._onData);
        this._port.once('close', () => { try { controller.close(); } catch { /* schon zu */ } });
      },
      cancel: () => { if (this._onData) this._port.off('data', this._onData); },
    });

    this.writable = new WritableStream({
      write: (chunk) => new Promise((res, rej) => {
        this._port.write(Buffer.from(chunk), (e) => {
          if (e) return rej(e);
          this._port.drain((e2) => (e2 ? rej(e2) : res()));
        });
      }),
    });
  }

  async close() {
    if (this._helper) {
      try { this._helper.stdin.write('quit\n'); } catch { /* schon tot */ }
      this._helper = null;
    }
    if (this._onData) this._port.off('data', this._onData);
    if (this._port?.isOpen) {
      await new Promise((res) => this._port.close(() => res()));
    }
    this.readable = null;
    this.writable = null;
  }

  // Web Serial: {dataTerminalReady, requestToSend} -> serialport: {dtr, rts}
  //
  // esptool-js setzt die Leitungen einzeln (reset.js ruft setDTR/setRTS getrennt),
  // Web Serial haelt die jeweils andere Leitung dabei unveraendert -> Zustand hier
  // mitfuehren.
  //
  // Fallback-Pfad: node-serialport ruft in set() unbedingt TIOCSBRK/TIOCCBRK, bevor
  // es TIOCMSET macht. cdc_acm (CH343 & Co.) kann kein break -> EOPNOTSUPP, und die
  // Leitungen werden nie gesetzt. Faellt set() so aus, uebernimmt der Python-Helfer
  // die beiden ioctls. Bei Treibern, die set() koennen (ftdi_sio u.a.), bleibt der
  // direkte Weg aktiv.
  async setSignals(signals = {}) {
    if ('dataTerminalReady' in signals) this._dtr = signals.dataTerminalReady;
    if ('requestToSend' in signals) this._rts = signals.requestToSend;

    if (!this._useHelper) {
      try {
        await new Promise((res, rej) =>
          this._port.set({ dtr: this._dtr, rts: this._rts }, (e) => (e ? rej(e) : res())));
        return;
      } catch {
        this._useHelper = true;   // einmal umschalten, nicht bei jedem Puls neu scheitern
      }
    }
    await this._helperSet(this._dtr, this._rts);
  }

  async _helperSet(dtr, rts) {
    if (!this._helper) {
      const { spawn } = await import('node:child_process');
      const { fileURLToPath } = await import('node:url');
      const helper = fileURLToPath(new URL('./tiocm-helper.py', import.meta.url));
      this._helper = spawn('python3', [helper, this.path], { stdio: ['pipe', 'pipe', 'inherit'] });
      this._helperLines = [];
      this._helperWaiters = [];
      let buf = '';
      this._helper.stdout.on('data', (d) => {
        buf += d.toString();
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, i).trim();
          buf = buf.slice(i + 1);
          const w = this._helperWaiters.shift();
          if (w) w(line); else this._helperLines.push(line);
        }
      });
      await this._helperLine();          // "ready"
    }
    this._helper.stdin.write(`${dtr ? 1 : 0} ${rts ? 1 : 0}\n`);
    const reply = await this._helperLine();
    if (reply !== 'ok') throw new Error(`tiocm-helper: ${reply}`);
  }

  _helperLine() {
    if (this._helperLines.length) return Promise.resolve(this._helperLines.shift());
    return new Promise((res) => this._helperWaiters.push(res));
  }
}

/** Sucht Pfad + VID/PID ueber die serialport-Geraeteliste (realpath-tolerant). */
export async function findPort(wanted) {
  const { realpathSync } = await import('node:fs');
  const target = realpathSync(wanted);
  const list = await SerialPort.list();
  const hit = list.find((p) => {
    try { return realpathSync(p.path) === target; } catch { return false; }
  });
  if (!hit) throw new Error(`Port ${wanted} (${target}) nicht in SerialPort.list()`);
  return {
    path: hit.path,
    info: {
      usbVendorId: parseInt(hit.vendorId ?? '0', 16),
      usbProductId: parseInt(hit.productId ?? '0', 16),
    },
  };
}

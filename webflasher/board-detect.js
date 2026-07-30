// SixBack web flasher — optional board identification.
// Shared by index.html and boards.html; both derive the button list from the
// DOM, so this file never needs to know which page it runs on.
// Extracted from index.html 2026-07-30 when the variants moved to boards.html.
// ---- Optional board identification -------------------------------------
  // esp-web-tools selects a build by chipFamily ALONE. That is not enough: a
  // 16 MB S3 image starts flashing onto an 8 MB board and dies partway
  // through, and on a 16 MB C5 the 4 MB image quietly leaves most of the part
  // unused. This helper reads chip family and flash size off the board with
  // esptool-js — the same library esp-web-tools uses internally — and marks
  // the button whose manifest tags actually fit ("sixback": minFlashSize,
  // psram; written by scripts/gen_manifests.sh).
  //
  // Deliberately advisory: it only adds labels and colours. The install
  // buttons stay clickable throughout, so a failure here can never stop
  // somebody from flashing.
  (() => {
    const ESPTOOL = 'https://unpkg.com/esptool-js@0.6.0/bundle.js';
    const btn = document.getElementById('detect-btn');
    const out = document.getElementById('detect-out');
    if (!btn || !out) return;

    const mib = s => { const m = /^(\d+)MB$/.exec(s || ''); return m ? +m[1] : 0; };

    if (!('serial' in navigator)) {
      btn.disabled = true;
      out.textContent = 'Board identification needs Web Serial — Chrome, Edge, Opera or '
        + 'Brave on a desktop. The buttons below still work if you know your board.';
      return;
    }

    // Derived from the DOM rather than hard-coded, so this list can never
    // drift away from the buttons actually on the page.
    const choices = [...document.querySelectorAll('esp-web-install-button[manifest]')]
      .map(el => ({ el, wrap: el.closest('.choice'), url: el.getAttribute('manifest') }));

    const label = (c, cls, text) => {
      if (!c.wrap) return;
      c.wrap.classList.add(cls);
      const s = document.createElement('span');
      s.className = 'verdict';
      s.textContent = text;
      c.wrap.appendChild(s);
    };

    async function evaluate(det) {
      for (const c of choices) {
        c.wrap?.classList.remove('fit', 'unfit');
        c.wrap?.querySelector('.verdict')?.remove();
        if (!c.manifest) {
          try { c.manifest = await (await fetch(c.url)).json(); } catch { /* keep going */ }
        }
        const m = c.manifest || {};
        c.build = (m.builds || []).find(b => b.chipFamily === det.chip);
        c.tags  = (c.build && c.build.sixback) || {};
        c.kind  = (m.sixback && m.sixback.kind) || c.url;
      }

      // Winner per kind (fresh install / update): the largest layout the
      // flash can still hold. See the minFlashSize note in gen_manifests.sh.
      const best = new Map();
      for (const c of choices) {
        const need = mib(c.tags.minFlashSize);
        if (!c.build || !need || need > det.flashMib) continue;
        const cur = best.get(c.kind);
        if (!cur || need > mib(cur.tags.minFlashSize)) best.set(c.kind, c);
      }

      for (const c of choices) {
        if (!c.build) { label(c, 'unfit', `not built for ${det.chip}`); continue; }
        const need = mib(c.tags.minFlashSize);
        if (need > det.flashMib) {
          label(c, 'unfit', `needs ${c.tags.minFlashSize} flash, your board has ${det.flash}`);
        } else if (best.get(c.kind) === c) {
          label(c, 'fit', '✓ use this one');
        } else {
          label(c, 'unfit', `${c.tags.minFlashSize} layout — smaller than your flash`);
        }
      }

      const pick = [...best.values()][0];
      const notes = [];
      if (pick && det.flashMib > mib(pick.tags.minFlashSize)) {
        notes.push(`SixBack uses a ${pick.tags.minFlashSize} layout on this chip, so the `
          + `remaining flash stays unused.`);
      }
      if (pick && pick.tags.psram === 'required' && det.psram === false) {
        notes.push('⚠ This image expects PSRAM and none was found on the board.');
      }
      out.innerHTML = `Detected: <b>${det.chip}</b>, <b>${det.flash}</b> flash`
        + (det.psram ? `, <b>${det.psram}</b>` : det.psram === false ? ', no PSRAM' : '')
        + (pick ? ` → use the buttons marked <b>✓ use this one</b>.`
                : ` — <b>none of the images below fit this board.</b>`)
        + (notes.length ? '<br>' + notes.join(' ') : '');
    }

    btn.addEventListener('click', async () => {
      let port, transport, loader, Reset;   // Reset is read in finally, keep it in scope
      // requestPort() must be the first await — it needs the click's transient
      // user activation, which a preceding dynamic import would spend.
      try {
        port = await navigator.serial.requestPort();
      } catch { return; }                       // user dismissed the picker

      btn.disabled = true;
      out.textContent = 'Talking to the board…';
      try {
        const { ESPLoader, Transport, UsbJtagSerialReset } = await import(ESPTOOL);
        Reset = UsbJtagSerialReset;
        transport = new Transport(port, false);
        const quiet = { clean() {}, writeLine() {}, write() {} };
        loader = new ESPLoader({ transport, baudrate: 115200, terminal: quiet });
        await loader.main();

        const chip  = loader.chip.CHIP_NAME;
        const flash = await loader.detectFlashSize();
        // PSRAM: the S3 target implements getPsramCap (eFuse-backed). The C5
        // has the same information in its eFuses — PSRAM_CAP, bits 21:19 of
        // EFUSE_RD_MAC_SYS2_REG (= EFUSE_BASE + 0x4C, ESP-IDF efuse_reg.h) —
        // esptool just never learned to read it, its C5 getChipFeatures() is
        // a hard-coded list. So read the register directly, like espefuse
        // does. Ground truth from an in-package-PSRAM C5 (C5HR8 die): the
        // word reads 0x21500310 -> PSRAM_CAP=2, and espefuse summary agrees.
        // 0 means "no in-package PSRAM". The value->size table is NOT
        // publicly documented for the C5 (unlike S3), so the raw value is
        // shown without a size claim. Chips where neither path applies
        // (C3/C6/classic ESP32) report null = unknown, never false.
        let psram = null;
        if (typeof loader.chip.getPsramCap === 'function') {
          const feats = await loader.chip.getChipFeatures(loader);
          psram = feats.find(f => /PSRAM/i.test(f)) || false;
        } else if (loader.chip.CHIP_NAME === 'ESP32-C5' && loader.chip.EFUSE_BASE) {
          const w = await loader.readReg(loader.chip.EFUSE_BASE + 0x4c);
          const cap = (w >>> 19) & 0x7;
          // Value->size measured, not documented: a C5HR8 die reading
          // PSRAM_CAP=2 initialises 8 MB (esp_psram_get_size, 2026-07-30).
          // Other non-zero values exist per the datasheet (C5HR2 = 2 MB) but
          // have not been observed here, so they stay as the raw value.
          const size = cap === 2 ? ' 8MB' : '';
          psram = cap === 0 ? false : `in-package PSRAM${size} (eFuse PSRAM_CAP=${cap})`;
        }

        if (!mib(flash)) {
          out.innerHTML = `Detected: <b>${chip}</b>, but the flash size could not be read`
            + ` (reported “${flash}”). Please pick the image by hand.`;
        } else {
          await evaluate({ chip, flash, flashMib: mib(flash), psram });
        }
      } catch (e) {
        out.textContent = 'Could not identify the board: ' + (e && e.message ? e.message : e)
          + '. Close any serial monitor using the port and try again — or just pick the image by hand.';
      } finally {
        // Leave the board running instead of parked in the download stub.
        // esptool-js auto-selects the USB-JTAG reset when it *connects*
        // (PID 0x1001, esploader.js), but after() always issues the classic
        // RTS pulse — which does nothing on a board attached through its
        // native USB port. So pick the matching reset here, or a C3/C6/C5
        // would sit in the stub until it is unplugged.
        try {
          if (transport?.getPid() === 0x1001 && Reset) await new Reset(transport).reset();
          else await loader?.after('hard_reset');
        } catch { /* best effort — worst case the board needs a power cycle */ }
        try { await transport?.disconnect(); } catch { /* best effort */ }
        btn.disabled = false;
      }
    });

    // Debug hook: lets the verdicts be exercised without a board attached, e.g.
    //   sixbackDetect.evaluate({chip:'ESP32-S3', flash:'8MB', flashMib:8, psram:false})
    // Useful when helping somebody who reports the wrong image being offered.
    window.sixbackDetect = { evaluate, choices };
  })();

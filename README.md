<p align="center">
  <img src="images/sixback-logo-crop-text.png" alt="SixBack — local SoundTouch cloud replacement" width="480">
</p>

# SixBack

> *Bring your six back.*

A tiny ESP32 stick that brings back the six Internet-radio preset buttons on
**Bose SoundTouch** speakers after Bose shut down their cloud
(2026-05-06).  It speaks just enough of the BMX cloud protocol that the
speaker firmware — which can no longer be updated — happily keeps working.

No subscription, no account, no Bose servers.  One USB stick on your LAN.

> SixBack was formerly developed and published as *BoseFix32*.  All
> functionality is preserved; the rename reflects the project's identity
> independent of any Bose trademark.

## What works today

- **The six preset buttons work again** — press 1-6 on the speaker and internet
  radio plays, with no Bose account and no Bose servers involved.
- **Presets you already had are kept** — they are imported from the speaker on
  first contact instead of being overwritten.
- **Radio, Spotify and DLNA** — search TuneIn and RadioBrowser, put a Spotify
  playlist on a preset button, or browse a DLNA/UPnP library and drag a track
  onto a slot.
- **Several speakers from one stick** — plus zones, multiroom and stereo pairs
  via the speaker's own protocol.
- **Nothing to host** — an inexpensive ESP32 board, flashed from the browser,
  running on your LAN. No subscription, no server, no account.

Full per-feature status and release history: **[STATUS.md](STATUS.md)**.

## Install (recommended)

Open the **web flasher** in Chrome or Edge desktop and click *Connect*:

> 🔗 **<https://sixback.io/>**

The page reads [`webflasher/manifest.json`](webflasher/manifest.json),
detects the chip family of the connected board, and writes the matching
factory image — bootloader + partition table + firmware + Web UI — in a
single shot.  Right after the flash, esp-web-tools also offers to hand
over WiFi credentials via Improv-Serial.

If Web Serial is unavailable, every target also ships an
`*-firmware.bin` (for OTA over WiFi) and `*-littlefs.bin` (for FS-OTA).

### ⚠ Auto-migration runs by default

A freshly-flashed device boots with **`auto_migrate_on_boot = true`** in NVS.
Once it is on your WiFi, it will:

1. Discover all SoundTouch speakers on the LAN (SSDP + ARP-probe).
2. For every eligible speaker (model whitelist `SoundTouch 10/20/30`,
   firmware whitelist `27.0.6.x` and `27.0.3.x`):
   - Read its current presets via the BMX API.
   - Normalize each preset (TuneIn passthrough; RADIO_BROWSER converted
     to a direct stream URL; DLNA / Bluetooth captured as opaque
     `<ContentItem>` and replayed 1:1).
   - Rewrite the speaker's cloud URLs via Telnet `:17000`.
   - Reboot the speaker; presets survive without long-press because the
     normalized list is embedded in the speaker's `account/full` sync.

If you'd rather drive each migration by hand, **turn the switch off at
the very top of `http://sixback.local/`** *before* the device finds your
speakers — or pre-disable it via `PUT /api/auto-mode` (Body:
`{"enabled":false}`).  The default is "on" because the typical install
path is *flash → provision → presets work*, and the foot-gun guards
(eligibility whitelists, `max_per_boot=4`) are tight enough that nothing
unrelated on your LAN gets touched.

After the initial boot pass, SixBack keeps the auto-mode pipeline alive
as a **periodic cron** (default every 30 minutes, configurable via
`cron_interval_s`).  Each tick does a light discovery (SSDP + known-IP
probe, no full `/24` sweep), runs Auto-Claim/Release on the inventory,
and migrates any newcomer that matches the eligibility whitelist.  A
speaker is only *released* when its new owner is verified — a live
SixBack peer answering on that URL, or an explicit revert to the Bose
cloud.  A speaker that points at a dead URL (a stale SixBack base after
an IP change, or a second stick that was retired) stays owned and is
automatically re-claimed on the next tick.  The countdown to the next
tick is visible at the top of the Web UI.

If multiple SixBack sticks coexist on the same LAN, the peer-aware
auto-mode (v0.7.5+) keeps them from fighting over the same speakers:
each stick HTTP-probes any foreign cloud URL it sees, and if the response
looks like another SixBack instance the speaker is left to its current
owner.  The UI labels such speakers as *claimed by peer @ &lt;ip&gt;*.

<p align="center">
  <img src="images/WebUIRadioSelector.png" alt="SixBack Web UI — radio/media selector with speaker preset slots" width="720">
</p>

The top of `http://sixback.local/` is where the **Auto-Migrate at Boot**
switch lives.  Below it every discovered speaker gets a card with its
current state (migrated / settling / original / foreign-cloud / offline),
its 6 preset slots, and per-speaker actions (migrate, revert, reboot,
edit presets, group sync).

## WiFi provisioning — two paths in parallel

On every cold boot the device opens **two** parallel provisioning
windows.  Whichever finishes first wins; the other is torn down.
Same pattern as the sister project [ip4knx / TUL KNX-Gateway](https://github.com/tostmann/ip4knx).

| Path           | When                                         | Window                                        |
| -------------- | -------------------------------------------- | --------------------------------------------- |
| Improv-Serial  | always                                       | 30 s idle (with creds) / 120 s idle (without) |
| Captive AP     | no NVS creds **or** STA-connect timeout      | 5 min idle                                    |

The **Improv** path is what esp-web-tools uses right after flashing.
The **Captive Portal** opens an **open** AP called `SixBack-XXYYZZ`
(no password) with a DNS hijack so any phone connecting to it gets the
WiFi-setup form automatically; after the user submits, the success
page auto-redirects to the device's freshly assigned LAN IP via
`<meta http-equiv="refresh">`.

## Supported hardware

| Chip          | Board reference                  | Flash  | Notes                                                            |
| ------------- | -------------------------------- | ------ | ---------------------------------------------------------------- |
| **ESP32-S3 ★**| `esp32-s3-devkitc-1` **with PSRAM** (any "R8" variant, e.g. N16R8 / N8R8) | ≥ 8 MB | **recommended** — **PSRAM is required** (TLS/HTTPS path for Spotify + OTA). The exact SKU is not important; clones are fine. 16 MB is the tested config and uses the default web-flasher button. **8 MB+PSRAM boards (e.g. Seeed XIAO ESP32S3) use the dedicated "S3 8 MB" button** on the web flasher (`s3-8mb` build: own partition table + own OTA channel) — do *not* use the standard S3 button on them, the 16 MB image does not fit the flash. **Since v0.8.35** the 16 MB S3 image answers Improv provisioning on **both** UART0 (the CH34x/CP210x bridge) and the chip's **native USB-Serial-JTAG** port — whichever one the host is plugged into; the first port that receives a valid Improv frame owns the session. A full web-flasher run (factory flash + *Connect device to Wi-Fi*) over the native USB socket has been verified end-to-end on a DevKitC-1, repeatedly. Two cosmetic notes: the browser console shows an *"Error fetching current state: TIMEOUT"* at ~88 % of the flash (the tool's provisioning check timing out while the chip is in download mode — the flash completes normally), and because the console log shares the native port with the handshake, the two can in rare cases interleave and make the browser report *"Improv Wi-Fi Serial not detected"* — retrying or using the UART port resolves it. Console logs stay on UART0. Boards that additionally require a manual bootloader-entry step before flashing are a separate question — see the Arduino Nano ESP32 row below |
| ESP32-S3 (Arduino&nbsp;Nano&nbsp;ESP32) | Arduino Nano ESP32 (S3 with 8 MB PSRAM) | 16 MB | works with the **standard S3 image and the default web-flasher button** — no build target of its own. The board exposes only the chip's native USB socket, which the dual-port Improv of v0.8.35 covers. Reported and tested on hardware by an external contributor in [#39](https://github.com/tostmann/SixBack/pull/39): flashing over the native port plus the *Connect device to Wi-Fi* step worked, and **updating** an already-running SixBack install needed no manual step. **A first install on a board still carrying the Arduino bootloader did require a manual bootloader-entry step**: `B1`→GND, press `RST`, remove the jumper — without it the web-flasher install aborts. The board has no BOOT button and `RST` alone does not help. Visible marker: in that state the port announces itself as *"Nano ESP32"*, and as *"USB JTAG/serial debug unit"* after the jumper step. Why the tool's reset sequence does not reach download mode in the first case is unresolved. OTA on this board is untested; it draws the standard `sixback-s3-*` artifacts |
| ESP32         | `esp32dev` (DevKitC-1)           | 4 MB  | classic — **shipped again** (v0.8.x); `scripts/fs_exclude_esp32.py` trims the Spotify-only `silence.mp3` from its LittleFS image so the gzipped Web UI fits the 256 KB spiffs slot |
| ESP32-C3      | `esp32-c3-devkitm-1`             | 4 MB  | flashes over the chip's built-in USB-Serial-JTAG                 |
| ESP32-C6      | `esp32-c6-devkitc-1`             | 4 MB  | WiFi 6 — works, but cold-start discovery occasionally drops SSDP-multicast packets and rare HTTP-server hangs need a reset |
| ESP32-C5      | `esp32-c5-devkitc1-n4`          | 4 MB  | **dual-band Wi-Fi 6 (2.4 + 5 GHz)** — native USB-Serial-JTAG; verified connecting on 5 GHz (channel 40; `band`/`channel` shown in `/api/status`). 4 MB / no-PSRAM devkit, A/B-OTA like C3/C6. **Note:** the C5 second-stage bootloader lives at flash `0x2000` (not `0x0`), and merging its factory image needs esptool ≥ 5. For 16 MB+PSRAM C5 modules see the dedicated row below |
| ESP32-C5 16 MB | modules with `ESP32-C5-WROOM-1-N16R8` (16 MB flash + 8 MB in-package PSRAM) | 16 MB | dedicated `c5-16mb` build target with the full 16 MB layout (`partitions.csv`, 3 MB A/B app slots + 9.9 MB LittleFS) and **PSRAM enabled** — lifts the tight-memory limits of the 4 MB C5 (Spotify active, larger page-load headroom) and has its own web-flasher button pair and OTA artifact channel (`sixback-c5-16mb-*`). The settings area (NVS) sits at the same flash offset in both layouts, so re-installing a 4 MB-layout stick with this image keeps WiFi credentials and presets. Use the web flasher's *Identify my board* button when unsure which C5 variant you have — it reads flash size and the PSRAM eFuse off the chip |

**S3 is the recommended target for distribution.** During the 4-phase
end-to-end test (S3 ↔ C6 ping-pong with full erase/flash/provision each
round) the S3 hit 3/3 speakers discovered + migrated in every single
auto-mode run, while the C6 needed a second boot in one cold-start case
and produced one HTTP-server hang that recovered only after a hardware
reset.  The extra ~5 € for an S3-DevKitC-1 (with PSRAM) buys noticeable
robustness and plenty of free flash for future features.  Any S3 board
**with PSRAM** works — the specific flash size is not critical (the app is
~1.6 MB and the web UI ~160 KB), but a board *without* PSRAM will struggle
on the TLS/HTTPS path and is not supported.

C3, C6 and ESP32-classic are fully functional and stay built/published on
every release.  ESP32-classic is published again: `scripts/fs_exclude_esp32.py`
strips the Spotify-only `silence.mp3` stub from its LittleFS image so the
gzipped Web UI fits the 256 KB spiffs slot of `partitions-4mb.csv`.

All targets share the same source tree and the same Web UI; the
PlatformIO `extends = common` mechanism keeps the per-target diff small
([`platformio.ini`](platformio.ini)).

## What it does on the speaker

After clicking *Migrate* in the Web UI, SixBack talks to the Bose
Diagnostic Shell on **TCP&nbsp;:17000** of the speaker and rewrites the
cloud URLs the firmware caches in NVS:

```
sys configuration bmxRegistryUrl http://<sixback-ip>:8000/bmx/registry/v1/services
sys configuration statsServerUrl http://<sixback-ip>:8000
sys configuration margeServerUrl http://<sixback-ip>:8000
sys configuration swUpdateUrl    http://<sixback-ip>:8000/updates/soundtouch
envswitch boseurls set http://<sixback-ip>:8000 http://<sixback-ip>:8000/updates/soundtouch
sys reboot
```

No SSH, no firmware mod, no Bose login.  The change is fully reversible
via *Revert to original Bose* — the speaker returns to its factory URL
set even though the original cloud is offline.

## Build locally

Requires PlatformIO and a Linux/macOS host.

```bash
# build everything (all targets) + LittleFS images
pio run -e esp32 -e s3 -e s3-8mb -e c3 -e c6
pio run -e esp32 -e s3 -e s3-8mb -e c3 -e c6 -t buildfs

# produce tagged factory images + manifest for the web flasher
./scripts/build_release.sh v0.8.22    # tag arg bakes the version into all firmwares

# flash a single target via USB
pio run -e s3 -t upload
pio run -e s3 -t uploadfs
```

Versioning + build snapshots are automatic
(see [`scripts/version_bump.py`](scripts/version_bump.py)): every local
build snapshots the working tree before bumping `build_number.txt`, so
you can always roll back to the exact state a given binary was built
from.  Those snapshot commits stay **local** — only tagged releases are
pushed to the public repo.

## Repository layout

```
src/                  Firmware (Arduino + ESP-IDF mix)
web-src/              Web UI source (index.html, gzipped at build time
                      into data/ for LittleFS)
webflasher/           esp-web-tools landing page + manifest (binaries
                      are .gitignored — rebuild via build_release.sh)
images/               README assets — title PNG + Web-UI screenshot
scripts/              version_bump pre-build hook + build_release.sh
partitions.csv        16 MB partition table  (ESP32-S3 16-MB modules)
partitions-8mb.csv    8 MB partition table   (ESP32-S3 8-MB modules, e.g. Seeed XIAO)
partitions-4mb.csv    4 MB partition table   (ESP32 / C3 / C6)
platformio.ini        Multi-env config, see `[common]` + `[env:*]`
```

## Support

SixBack is free and open source. If it kept your speakers out of the
landfill and you'd like to say thanks, there's a tip jar via
[PayPal](https://paypal.me/busware) — entirely optional, and it helps keep
the lab stocked with test hardware. A ⭐ on the repo is just as welcome.

## Acknowledgements

- **[atomicobject/heatshrink](https://github.com/atomicobject/heatshrink)** (v0.4.1, ISC) —
  embedded LZSS compressor vendored under `src/heatshrink/`; SixBack uses it
  to compress the NVS-persisted JSON stores (presets, inventory, libraries)
  with a 1.6 KB encoder state, raising the per-stick speaker ceiling.
- **[julius-d/ueberboese-api](https://github.com/julius-d/ueberboese-api)** —
  OpenAPI specification of the legacy Bose SoundTouch streaming cloud,
  reconstructed from observed traffic. It is SixBack's verifiable
  ground-truth for endpoint shapes, header semantics, and event-body
  formats (scmudc envelope, NowPlaying structure, kebab-case event
  types, group/preset XML).  Thanks to **julius-d** for publishing it.

- **[tostmann/ip4knx](https://github.com/tostmann/ip4knx)** — sister
  project. The dual-path WiFi provisioning (Improv + Captive in
  parallel) and the system-health / self-ping watchdog pattern are
  carried over from there.

## Disclaimer

SixBack is an independent open-source project.  It is **not** affiliated
with, endorsed by, or sponsored by Bose Corporation.  All references to
Bose products and protocols are nominative, for interoperability with
hardware their owners have already paid for.  Use at your own risk.

## Licence

[PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0).
See [LICENSE](LICENSE) for the full text and
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for upstream
component licences.

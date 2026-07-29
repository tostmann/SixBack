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

## Status (v0.8.36)

| Component                                                            | State                                                                                                              |
| -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| **Web-flasher "Update existing" no longer erases the device (v0.8.36)** | fixed — the update button could wipe the whole flash before writing, which left the stick unable to start at all, because the update package deliberately contains only the app and the filesystem and no bootloader. Cause: esp-web-tools decides whether to erase by comparing the firmware name the device reports over Improv against the manifest's `name` field, as an exact string match. The update manifests were named `SixBack (update)` while the device reports `SixBack`, so the match always failed and the tool treated the update as an install of foreign firmware. The manifest option meant to catch this is easy to misread: `new_install_prompt_erase` does not mean "erase", it means "ask before erasing" — set to `false` it erased **without asking**. Both update manifests now carry `SixBack` verbatim so the comparison matches and no erase happens, and the option is set to `true` as a safety net for the case where the provisioning window has already closed and the firmware name cannot be read (the dialog's checkbox defaults to unchecked, so that path does not erase either). Reproduced on hardware before fixing — erase followed by the update parts leaves a device looping on `invalid header: 0xffffffff` — and this also explains field reports of "update existing, then unreachable, and a power cycle does not help, only a fresh install". The corrected manifests are already live; the landing page no longer claims a fresh install "erases all settings" and tells you not to tick the erase box |
| **Presets take ~12× less storage (v0.8.36)**                         | fixed — the NVS partition holds 630 entries for *everything*: Wi-Fi credentials, the speaker inventory, the preset store, Spotify auth, hostname, auto-mode config. Measured on hardware, a TuneIn preset carrying both a stream URL and a logo URL cost about 6 entries, and the store hit `partition out of space` at **5 speakers** — inside the range this project targets, which is why users with more speakers saw presets silently fail to persist and reappear empty after a reboot. Neither field is needed for TuneIn: every consumer builds the playback location from the station ID itself, and the logo URL is almost always the canonical one derivable from that same ID. The stream URL is therefore no longer stored for TuneIn presets, and a canonical logo URL is omitted and reconstructed on load. Nothing is lost in the process: a custom logo URL is still stored verbatim, a deliberately empty one stays empty, presets written by earlier firmware are read back unchanged, and exports keep both fields so a backup stays self-contained. Other sources are untouched — for local internet radio the stream URL *is* the content and is still persisted. Verified across reboots for all of those cases; the same test load that previously failed at 5 speakers now reaches 22 at half the partition. Existing presets shrink the next time they are written, so re-saving a slot is what applies the gain to an installed device |
| **Provisioning over either USB socket on the 16 MB S3 (v0.8.35)** | new — boards of the ESP32-S3 family come in two wiring styles: with a CH34x/CP210x UART bridge (e.g. DevKitC-1, two sockets) and with only the chip's native USB-Serial-JTAG (a single socket). Which one carried the Improv provisioning handshake used to be a **compile-time** decision, so a board without a bridge chip needed its own build target. The 16 MB S3 image now answers the handshake on **both** ports at once: two independent Improv instances, and the first port that receives a valid `IMPROV` frame owns the session (the guard matches on the protocol magic rather than the first byte, so stray terminal output on one port cannot lock out a real host on the other). Console logs stay on UART0, unchanged. Verified end-to-end on hardware: a complete web-flasher run — factory flash plus the *Connect device to Wi-Fi* step — over the native USB socket, twice; each port also answers alone; and both are live within a single boot window. Robustness measured with checksum validation rather than frame-scanning: 894 Improv frames under sustained request load, with ~11 KB of concurrent console output interleaved into the same stream, zero corrupted frames. Safe for existing installs: the bootloader binary is byte-identical between the previous release and this build, so the underlying flag is purely app-level and cannot break OTA. One caveat: a single flash attempt over the native port aborted at the reset step before writing anything and has not reproduced since — command-line `esptool` flashes that port without trouble, so it looks browser-side; retry, or use the UART socket |
| **Configurable mDNS / DHCP hostname (v0.8.35)**                      | new — running two sticks on one network meant both announced themselves as `sixback.local`, which collides in mDNS and makes the DHCP lease list ambiguous. The hostname is now configurable at runtime (`GET`/`PUT /api/hostname`, plus a field in the Web UI's settings bar) and is applied to **both** the mDNS announcement and the DHCP client name. Server-side validation accepts `a-z`, `0-9` and `-`, up to 24 characters, lower-cases the input, and an empty value resets to the default; the API reports the persisted name, the one active since boot, and whether a reboot is still pending, so the UI cannot show a value the device is not actually using yet. Verified end-to-end including name resolution from a separate host after reboot, and a clean reset back to the default |
| **Hide individual speakers (v0.8.35)**                               | new — on a network with speakers that belong to somebody else (a shared flat, a neighbouring system), the discovery list showed every SoundTouch it could find, with no way to get the irrelevant ones out of the way. Speakers can now be hidden per device (`POST /api/speakers/hide`); hidden entries disappear from the list and from every picker (zone, stereo pair, media browser, group sync), and their live-status polling is skipped, which also removes their share of the network fan-out. The flag survives re-discovery and reboots. Deliberately **display-only**: the backend keeps serving a hidden speaker normally, so hiding never changes what the device does for it — only what you see |
| **Store-save failures are now visible (v0.8.35)**                    | new — a preset store that fails to persist used to be silent in the UI: the running device looked fine and the loss only showed after a reboot. The header now carries a warning pill, and the system panel a dedicated row, whenever the store reports failed saves or an unreadable blob (`preset_store.save_fails` / `load_ok`, both already exported by the firmware since v0.8.33). The inventory got the same treatment (`inventory.save_fails`), so a device that cannot write its speaker list says so instead of quietly forgetting it after the next power cycle |
| **Spotify hardware-button watcher — runtime PSRAM check (v0.8.35)**  | fixed — the watcher that re-arms cold Spotify slots for the hardware preset buttons was gated at **compile time** on the S3 targets, which assumed every S3 board carries PSRAM. On an S3 module without it (an R2 or FN8 variant flashed with the standard image) the watcher would start into an environment it cannot support. It now checks the actual PSRAM size at runtime and returns before allocating anything, following the same pattern the Spotify subsystem already uses. Boards with PSRAM are unaffected — verified on hardware, the watcher starts and connects as before |
| **Preset store — crash-safe A/B persistence (v0.8.34)**              | new — lab work on the sporadic "all presets gone overnight" reports (FHEM #144729) found the actual loss mechanism: once the store grows past what the NVS partition can hold, a failing multi-chunk blob write can leave the stored blob **inconsistent**, and the NVS init at the *next boot* silently discards it — the device then starts with an empty store that looks exactly like a fresh install, so no error is ever shown. Reproduced twice on real hardware with a fill-past-capacity + delete-storm sequence: the entire store (including speakers that were never touched) was gone after reboot. The store (and all other critical settings blobs) is now written **double-buffered**: every save goes to the inactive of two slots, is read back and byte-compared, and only then does an atomic generation counter make it the active one — a failing or interrupted write can only ever hit the spare slot, never the last good state. The loader falls back to the other slot if the active one is unreadable. The previous last-resort recovery path (erase-then-rewrite of the live key), which could destroy the last good state under repeated failing saves, is gone entirely. The identical kill sequence now leaves every untouched speaker's presets intact across reboots, and the partition no longer degrades (saves past capacity fail cleanly with an honest error instead). Existing stores migrate transparently on their first save; capacity is unchanged (~9 speakers on the 20 KB NVS partition — for more, split the fleet across two devices) |
| **Web-UI page load — concurrency guard (v0.8.34)**                   | new — on no-PSRAM boards, **two simultaneous downloads of the ~75 KB Web-UI bundle** (two tabs, or two devices opening the UI at once) could exhaust the heap mid-transfer faster than any request-time check can react: the Wi-Fi stack fails to allocate and the board goes **unreachable until reset** (reproduced on a C5: dead within ~2 s, 94 % ping loss, no self-recovery — while a single tab is completely stable even in a tight reload loop, 256 consecutive full loads, zero loss). The UI bundle is now served **one transfer at a time** on no-PSRAM chips (three in parallel with PSRAM); an additional simultaneous request gets a tiny auto-retrying "busy" page instead of a competing 75 KB stream. Same double-load test after the fix: hundreds of completed full transfers, zero ping loss, and the page still loads normally afterwards. Extreme synthetic request floods (a DoS-class hammering no browser produces) can still exhaust a no-PSRAM board — the realistic multi-tab case is what this closes |
| **Per-speaker reads rejected at healthy heap levels (v0.8.34)**      | fixed — the outbound-concurrency guard kept a *total-free-heap* floor of 45 KB on no-PSRAM chips, calibrated before v0.8.32 introduced hard timeouts on the *per-speaker* proxy reads. With those timeouts the worst-case drain is bounded, and 45 KB turned out to be inside the **normal operating range** of a loaded no-PSRAM device — reported in FHEM #144729 as a box showing "low heap" with 45–62 KB free. The floor is now 30 KB (measured against real C5 behaviour: normal UI load dips to ~60 KB, and 8.7 h of stock operation bottomed out at 28.7 KB without incident, so the old floor only ever blocked legitimate reads). Because a rejected read shows the same "device busy (low heap)" message for **three different causes**, `/api/status` now breaks rejections down by cause (`heap.rej_floor_total` / `rej_floor_block` / `rej_inflight`) whenever any occurred — the next field report can tell a genuine heap problem from the two-reads-at-a-time limit colliding with a slow-to-answer speaker |
| **Preset reads rejected on fragmented heaps — v0.8.32 regression (v0.8.33)** | fixed — v0.8.32 switched the outbound-concurrency guard from *total* free heap to the **largest free block**, but kept the threshold that had been calibrated against the total (45 KB on no-PSRAM chips). The largest contiguous block is always smaller than the total — on a no-PSRAM chip the ordinary Web-UI page-load burst alone pushes it below that bar, so nearly every per-speaker read was rejected with *"device busy (low heap)"* even though total free heap was healthy (reported in FHEM #144729 on an ESP32-C5 with ~70 KB free). Reproduced on real C5 hardware: single reads passed, but the parallel burst a page load fires was rejected 17/18. The guard now checks **both** conditions with their own thresholds: total free ≥ 45 KB (the collapse protection from v0.8.30/31, unchanged) **and** largest block ≥ 16 KB (what one read actually needs contiguously). Same burst after the fix: all reads pass, zero ping loss, no crashes. `/api/status` gained `heap.largest_block` and `heap.outbound_rejects`, so this failure mode is visible in a status dump instead of needing guesswork |
| **Preset-store forensics + wipe protection (v0.8.33)**               | new — until now, a stored preset blob that could not be read back at boot was **completely silent**: the store just started empty, and on the next preset sync the speakers could interpret the empty answer as "this account has no presets" and clear their local preset memory. Three changes close that path: (1) an unreadable blob is now detected, logged, and reported (`/api/status` → `preset_store.load_ok` / `save_fails` / `speakers`); (2) while the stored presets are unreadable the device answers the speakers' preset sync with **404 — speakers keep their local preset cache** — until the first successful save replaces the broken blob (a fresh install without stored presets is unaffected); (3) a save that would silently truncate under memory pressure is aborted before writing, keeping the last good state in flash. Verified on real hardware with a deliberately corrupted store blob: detection, the protective 404, recovery by writing a preset, and persistence across reboots all confirmed. This is groundwork for the sporadic "all presets gone" reports in FHEM #144729 — it makes the next occurrence diagnosable from `/api/status` and prevents the known wipe path while the cause is investigated |
| **WebUI under load — shorter blocking, fewer outbound calls (v0.8.32)** | fixed — opening the Web UI fires one live-status request per speaker, and each of those ran a **synchronous** outbound HTTP GET inside the single async web-server task. `handleNowPlayingLive` set **no timeout at all** (so a standby speaker could block on the HTTPClient default of ~5 s connect + ~5 s read) and the hardware-preset fetch used 3 s — with a handful of speakers, or a second browser tab, the server task stalled long enough that the whole device went unreachable, on the smallest-SRAM chip up to a full Wi-Fi blackout without any reboot. Both live handlers now use tight timeouts (600 ms connect / 800 ms read — measured speaker replies are well under 60 ms, so the margin is >13×), a **2.5 s micro-cache** per speaker+endpoint collapses parallel tabs and the per-speaker fan-out into at most one outbound call per window, and the concurrency guard now measures the **largest free block** (`heap_caps_get_largest_free_block`) instead of total free heap, which is what an allocation actually needs. `/api/status` gained `heap_low_events` + `last_heap_low_s` (runtime-only, no flash writes) so heap pressure is visible in the field. Measured under an identical synthetic flood on real hardware: S3 served **6× more** requests from the backlog (16 → 99) with **6× fewer** timeouts (70 → 11) and no ping loss in either build; a C5 improved but still went deaf under the same load — see the ESP32-C5 row below (FHEM #144729) |
| **Spotify bindings — auto-cleanup + remove button (v0.8.32)**        | fixed — a slot's Spotify binding and its Bose preset are two separate layers: dragging a playlist onto a slot writes the binding **and** a TuneIn "tunnel" preset (`🎵 <playlist>`, the mechanism that lets a hardware button start Spotify at all). Overwriting that slot with an ordinary station replaced only the preset — the binding stayed behind, kept showing as a green sub-row under the slot, and other clients reading the speaker (e.g. FHEM BOSEST) still saw the old playlist name. It did nothing (an occupied slot plays its station and ignores the binding), but it was misleading and there was no way to get rid of it. Writing a real preset over a bound slot now clears the stale binding automatically — server-side, so it holds for every client, not just the Web UI — and the Spotify sub-row gained a **✕** to remove a binding by hand, leaving the preset untouched. Deliberate exceptions: the tunnel write itself, preset imports that carry a Spotify mapping, and an empty slot with a binding (which powers click-to-play in the UI) (FHEM #144729) |
| **Wi-Fi drop diagnostics + silent-disconnect detection (v0.8.31)**  | working — the Wi-Fi watchdog now treats the link as *up* only when the station is associated **and** holds a valid IP. A silent drop where the driver keeps reporting associated at `0.0.0.0` (a lost DHCP lease / failed renew) was previously invisible to a bare `WL_CONNECTED` check, so no reconnect fired; it is now detected and drives the periodic reconnect. `/api/status` gained two health fields — `wifi_disconnects` (a persistent count of detected link losses, even ones that never triggered a reboot) and `last_wifi_down_s` (duration of the most recent outage) — so a single status read after a drop shows whether the device merely lost the link or actually rebooted, without a serial cable. The diagnostic snapshot also carries a new `device` block (chip / PSRAM / heap / Wi-Fi / health) so an uploaded snapshot includes the board identity and health inline. Groundwork for diagnosing a reported WebUI-triggered Wi-Fi drop on a dual-band board (FHEM #144729) |
| **No-PSRAM heap headroom + WebUI stability under load (v0.8.30)**   | fixed — on the no-PSRAM chips (C5 / C6 / C3) the Spotify subsystem eagerly allocated its TLS clients and a worker task at boot even though Spotify needs PSRAM to run at all, wasting ~18-22 KB of heap. Under a full WebUI page load (a fresh browser tab firing the page bundle + several `/api` calls + the per-speaker probe fan-out at once) the tight heap could collapse, crash, and drop the Wi-Fi link — the page never finished loading and presets did not appear. `spotify::init()` now skips the eager allocation when there is no PSRAM (S3 with PSRAM is unchanged), and the page-serving / refresh-status endpoints gained heap-floor guards as a safety net under heavy concurrency. Verified on a no-PSRAM C5: a single-tab load no longer crashes (3/8 → 0/8) and the S3 shows no regression (FHEM #144729, #121) |
| **Auto-Migrate toggle persistence + honest save (v0.8.29)**         | fixed — the **Auto-Migrate at Boot** switch could silently fail to persist: `PUT /api/auto-mode` always reported success even when the underlying NVS write failed, so the Web UI showed the new state while nothing was stored — after a reboot (or the UI's immediate re-read of the persisted value) the switch snapped back. The endpoint now uses the same **cleanup-retry NVS path** as the other settings stores (purges regenerable caches on a full partition, then retries) and **reports the real result** — a failed persist returns HTTP 500 so the UI surfaces it instead of a false success. The stereo-pair group store got the same cleanup-retry save, and `/api/status` gained a `groups_persist_ok` health flag (kept as diagnostics, the speaker-facing BMX responses are unchanged). Reproduced and verified end-to-end on a freshly-erased C5 — a non-default toggle value now survives a reboot (FHEM #144729) |
| **Refresh status — non-blocking (v0.8.27)**                         | working — the global **🔄 Refresh status** used to run the entire inventory probe (a per-speaker Telnet/BMX status check, an up-to-30 s stale-view retry, plus a Spotify-source pull per speaker) **synchronously** inside the single HTTP request the browser was waiting on with no timeout — so on a large speaker zone the whole Web UI froze at the first step. The probe now runs in a **background task** (the same pattern as **Discover**): the request returns immediately and the UI polls `/api/speakers` until it finishes, staying responsive throughout. The HTTP helper also gained a request timeout so no single call can hang forever. Reported on the FHEM forum |
| **Optional WebUI password protection (v0.8.27)**                    | working — an optional **HTTP Digest** login can be turned on for the device Web UI and its `/api` (off by default; configured via `GET/PUT/POST /api/ui-auth`). It gates only the Web UI / API surface — the BMX / cloud-replacement endpoints the speakers themselves talk to stay open, so device compatibility is unaffected. On a trusted LAN this is deterrence rather than a substitute for network isolation, since the traffic is plain HTTP (#31) |
| **Clock display on/off — ST20 / ST30 (v0.8.25)**                    | working — speakers that have a display (ST20 / ST30) get a 🕒 **Clock display** on/off switch on their card. SixBack reads and writes the speaker's device-direct `GET/POST :8090/clockDisplay` (nested `<clockConfig>` envelope, `text/xml`), gated on the speaker's `capabilities.xml` `<clockDisplay>` flag so it only appears on speakers with a display (the ST10 has none). The write is read-modify-write, so the other clock-config fields (timezone / format / offset) are preserved. Verified on real ST30 hardware — the display is **binary on/off** (it cannot dim, so there is no brightness control) |
| **STORED_MUSIC source self-heal (v0.8.25)**                         | working — a DLNA / STORED_MUSIC preset push could fail (`/select=500`, "speaker hardware out of sync") after a power outage rebooted the whole network: the speaker came back before its DLNA media servers were rediscovered, so its `STORED_MUSIC` source was never re-registered — while TuneIn (always-on) and DLNA **browse** kept working, which is exactly the reported symptom. The periodic status check now probes for a READY `STORED_MUSIC` source on owned, migrated speakers that have media servers, and on miss re-runs `refreshMediaServers` + `/setMargeAccount` to re-register it — the same self-healing the TuneIn source already had. Idempotent across cron ticks; verified on real hardware (#30) |
| **Hardware preset buttons — re-arm cold LIR slots (v0.8.23)**       | working — a physical preset-button press for a Local-Internet-Radio slot is now detected in real time over the Bose **`gabbo` notification WebSocket** (`ws://<speaker>:8080/`, LAN-reachable via `wsapiproxy`). When a cold slot fails to reach *playing* state — the raw-URL location yields no `playStatus`, or the speaker reports `INVALID_SOURCE` — SixBack re-arms it through the native ORION `/select` (the same path the UI **Play** button uses, v0.8.20), now **hardware-triggered**, so the physical button just works again. A persistent hand-rolled RFC6455 WebSocket client (zero deps) is kept per owned speaker *that has LIR presets*, with a 3-layer loop-guard against re-select storms (failure-only trigger · 20 s self-select suppress · per-slot attempt cap) and an active liveness ping + idle-reconnect. **S3 / S3-8MB only** (PSRAM); compiled out (no-op) on C3 / C6 / classic (#15) |
| **WebUI security headers (v0.8.23)**                                | working — the device Web UI now ships a **Content-Security-Policy** + `X-Content-Type-Options: nosniff` as defense-in-depth against XSS from untrusted data rendered in the UI (RadioBrowser / TuneIn / DLNA station names, custom stream URLs). The single-file SPA is inline-heavy, so `script-src` keeps `'unsafe-inline'`; the real hardening is `frame-ancestors 'none'` / `object-src 'none'` / `base-uri 'self'` / `form-action 'self'` + https-only `img` / `connect` |
| **Speaker hardware fingerprint (v0.8.23)**                          | working — the speaker's `moduleType` (sm2 / scm wireless-module generation) and `variant` (rhino / mojo / spotty) are parsed from `/info` and surfaced in `/api/speakers`, so issue triage can tell apart hardware revisions that `model` (e.g. *SoundTouch 20*) hides |
| Cloud replacement (`/bmx/registry`, `/streaming/…`, `/updates/…`)    | working — 22 / 30 ueberboese-spec endpoints served                                                                 |
| **ESP32-S3 8 MB variant — Seeed XIAO ESP32S3 & similar** (v0.8.18)  | working — dedicated `s3-8mb` build target with its own partition table (`partitions-8mb.csv`, same 3 MB A/B OTA app slots as the 16 MB layout, 1.9 MB LittleFS), its own web-flasher button pair and its own OTA artifact channel (`sixback-s3-8mb-*`), so over-the-air updates always match the flashed layout. Spotify stays enabled (8 MB PSRAM). The OTA puller also gained a **pre-flight size check** on all targets: an image larger than its target partition is refused before anything is unmounted or written ("wrong build variant for this board?") (#23) |
| **Rename speakers from the WebUI** (v0.8.18, fixed v0.8.22)          | working — ✏ next to the speaker name renames it app-free, so the new name persists across reboots. SixBack sends the rename device-direct (`POST :8090/name`, which the speaker silently ignores unless the content type is `text/xml`). A **migrated (cloud-bound) speaker does not apply the rename locally — it delegates it to its cloud**, firing `PUT /streaming/account/{a}/device/{deviceId}` back at SixBack. v0.8.22 adds the handler for that callback and records the new name, so renames now stick on migrated speakers too — before v0.8.22 SixBack ignored the callback and the rename silently did nothing (earlier mis-attributed to a non-existent "one rename per power-on" device limit). The UI re-checks the speaker's actual name a few seconds later instead of trusting the HTTP status, and flags it if the rename didn't take. The System tab also shows the flash size (16 MB / 8 MB) so you can tell the S3 variants apart (#25) |
| **Spotify — Library + slot trigger** (v0.7.7 → v0.7.11)              | working — connect once via OAuth in the 🎵 Spotify sidebar tab, save tracks / albums / playlists as reusable **Library tiles** (device-side NVS, `GET/POST/DELETE /api/spotify/library`), then drag a tile onto a preset slot. A physical button press fires the Spotify Web-API `/play` to the speaker as a Connect device, with per-slot **shuffle** + **repeat** (one track / full album-playlist) and a live trigger log with 🎵-badges |
| **Media sidebar — search & drag onto slots**                        | working — a 4-tab Media panel (📻 Radio · 🔗 Stream · 🎵 Spotify · 💿 DLNA): search TuneIn / RadioBrowser stations, keep custom stream URLs and Spotify Library tiles, or browse DLNA servers, then drag any result straight onto one of a speaker's 6 preset slots |
| **Marge keep-alive** (v0.7.7)                                        | working — 60s background ping of `/setMargeAccount` to every known speaker; prevents the scmudc event-stream from going silent after hours of idle |
| **Marge pair-bootstrap** (`/setMargeAccount` round-trip)             | working — `/streaming/account/{a}/device/` echoes deviceid with Bearer-credentials                                 |
| **scmudc telemetry** — per-device NowPlaying + event trace           | working — body-captured `/v1/scmudc/{deviceId}` JSON parsed into per-speaker store                                 |
| TuneIn preset resolver (`Tune.ashx` + `Describe.ashx`)               | working — stations show with correct name & artwork. **AAC-only stations now play (v0.8.9):** without an explicit `formats=` filter TuneIn hands back a `notcompatible.enUS.mp3` placeholder for AAC-only stations (the speaker played a ~12 s "station not compatible" message, then stopped). The resolver now requests `formats=mp3` first (keeps the most-compatible stream for dual-format stations) and falls back to `formats=aac`, so AAC-only stations resolve to their real stream — the SoundTouch decodes AAC-LC and HE-AAC v1/v2 fine; HLS (`.m3u8`) variants are skipped. `POST /api/tunein/cache/clear` flushes stale cached resolutions after updating. |
| Preset push to speaker — serialized FreeRTOS queue (v0.6.0)          | working — single persistent worker drains pushes one-by-one; depth 16, 503 when full; refuses with an actionable HTTP 409 ("migrate this speaker first") when the speaker isn't migrated yet, instead of a confusing "didn't save" (v0.8.7); waits for the speaker to actually reach *playing* state before the long-press (up to ~18 s) so a slow stream-start no longer drops the preset (v0.8.8) |
| **Source self-healing** (v0.8.8)                                     | working — a migrated speaker whose SixBack account never bound (empty `margeAccountUUID` → no TuneIn/Spotify/DLNA sources registered, so every push failed with `/select=500`) is detected on the periodic status check and **auto-re-bound** (synthetic per-device account id + `/setMargeAccount`), re-registering its sources with no user action; a `⚠ sources not synced` badge + a **Re-Sync Sources** button surface it in the WebUI too (#10) |
| **Captive portal** — WiFi setup AP                                   | working — fixed the `ERR_TOO_MANY_REDIRECTS` redirect loop that broke the setup page (the root route was a regex pattern handed to a non-regex router); the portal now loads cleanly (v0.8.8, #12) |
| **Compressed NVS stores** (v0.8.17)                                  | working — the JSON stores (presets, inventory, stream/Spotify libraries) are now heatshrink-compressed in NVS (vendored [atomicobject/heatshrink](https://github.com/atomicobject/heatshrink) v0.4.1, 1.6 KB encoder state, measured factor ~2-4 on real store data), raising the per-stick ceiling from ~7 fully-loaded speakers to 15-20+. Values under 512 B stay plaintext; legacy stores migrate in place on first save; every decode path is fail-safe (fuzz-tested host-side, 2700 cases under ASan/UBSan) and a corrupt frame can never hang the boot. **Note:** firmware older than v0.8.17 cannot read compressed stores — after a manual downgrade the preset store re-seeds from the speakers, but stream/Spotify library tiles would need a re-import |
| **Preset / inventory store — NVS blob storage** (v0.8.15)           | working — both per-stick stores (preset assignments, speaker inventory) were single NVS *strings*, which hit ESP-IDF's hard 4000-byte `nvs_set_str` limit at roughly five speakers with full presets: from then on **every** save failed regardless of free space (the "partition full" error was misleading — the partition was two-thirds empty), and a cleanup pass could destroy the last persisted state, so presets vanished on the next reboot or OTA update. Both stores are now NVS **blobs** (page-chunked, limit = partition size), the cleanup backs up and restores the previous value instead of sacrificing it, and a genuine out-of-space reports an accurate error. Existing data migrates in place on first save. Practical ceiling on the 24 KB NVS partition is ~7 fully-loaded speakers per stick — beyond that, writes fail loudly but never destroy data |
| **Preset-loss defense** (Defense-in-Depth)                           | working — `handleMigrate` pre-imports; `/presets` and `account/full` return 404 when store empty; TUNEIN source-block carries `username=TuneIn` so `sourceAccount` survives every sync |
| **Opaque-source passthrough** — DLNA / UPnP / Bluetooth presets      | working — original `<ContentItem>` captured at import and replayed 1:1; `STORED_MUSIC` and `STORED_MUSIC_MEDIA_RENDERER` declared in `accountSources`; serialized as Bosman-schema `<preset>` blocks with `<location>` + `<source>` reference (v0.6.537) |
| **DLNA preset workflow** end-to-end                                  | working — verified on SoundTouch 30 with 6/6 OPAQUE slots reboot-persistent (2026-05-21)                           |
| **DLNA browse** in the WebUI (v0.8.0)                                | working — sidebar tab with speaker + server pickers, breadcrumb, drag-track-onto-slot; UPnP ContentDirectory:Browse SOAP runs in a small Pi5/Apache-fronted proxy so the firmware stays thin; tested against MiniDLNA, Fritz!Mediaserver |
| **DLNA preset recording** via drag-to-slot (v0.8.0)                  | working — `POST /api/speaker/{id}/dlna/preset/{slot}` emulates long-press (`/select` STORED_MUSIC ContentItem → 8 s settle → `/key` press+release) then re-imports `/presets` so the new OPAQUE slot is captured into the store with its `rawContentItem`; peer-aware refuse (HTTP 409) when the speaker is owned by another SixBack |
| **Migrate / Reboot progress modal** (v0.8.0)                         | working — both speaker actions open the same step-by-step progress dialog used by Refresh; status transitions are tracked by polling `/api/speakers`, with explicit timeout + last-status surfacing if the speaker never returns |
| Speaker telnet bootstrap (`sys configuration …` via TCP 17000)       | working                                                                                                            |
| **Migrate verify post-boot** (v0.7.632)                              | working — second `getpdo` after `waitForSpeakerBack_`; mismatch → `MIGRATE_FAILED` instead of silent `MIGRATED`    |
| **Migration robustness** (v0.8.21)                                  | working — the Telnet `:17000` migration bootstrap retries up to **3× with backoff** before giving up, and on persistent failure surfaces a clear *weak-WiFi* message with the speaker's **RSSI** instead of a generic timeout; CPU clock is set to each chip's maximum (e.g. C6 → 160 MHz) for snappier TLS/HTTPS work (#28) |
| Auto-import existing presets via BMX `/presets`                      | working                                                                                                            |
| **Stereo-Pair / Multi-Room group API**                               | working — POST/PUT/DELETE on `/streaming/account/{a}/group/`, NVS-persistent                                       |
| **Stereo pair — ST10 left/right pairing in the WebUI** (v0.8.16)    | working — SoundTouch 10 cards get a stereo-pair row: pick the right-channel speaker and the two ST10 join into one left/right stereo image that presents as a single device (`POST /addGroup` to the master, device-direct — the speaker itself registers the pair with the SixBack cloud store, so it survives ESP reboots). Un-pair works from either card (`/removeGroup` is routed to the master). ST10-only per the protocol (`supportedURLs` is *not* a reliable gate — an ST30 advertises `/addGroup` too — so the UI gates by model). Stale pair entries left behind by the Bose app's own un-pair path are now pruned when a new pair is registered (#22) |
| **Device-direct multiroom** (ZoneManager, v0.8.7)                    | working — group speakers straight through the speaker's own `/setZone` / `/getZone` on port 8090 (master + slaves); stateless, live truth read from the master's `/getZone` — a separate layer from the cloud group-store above; WebUI group-picker / badge / ungroup |
| **Auto-Mode** — discover + migrate + preserve presets on first boot  | working — gated by NVS flag, default on                                                                            |
| **Auto-Mode cron** — periodic re-check every 30 min when enabled     | working — light discovery + auto-claim/release + migrate newcomers; since v0.8.13 a speaker is only *released* to a **verified** foreign owner (a live SixBack peer, or an explicit revert to the Bose cloud) — a speaker pointing at a dead URL stays owned and is **re-claimed** automatically (covers stale bases after an IP change and retired second sticks; the re-claim path skips the model/firmware whitelist because the speaker has already been migrated successfully before) |
| **Peer-aware Auto-Mode** (v0.7.5)                                    | working — HTTP-probes other SixBack sticks in the LAN; skips speakers already claimed by a peer; UI shows `claimed by peer @ <ip>` |
| **Source-Normalizer** — TuneIn / Local / RadioBrowser → playable     | working — RadioBrowser UUID resolved via radio-browser.info                                                        |
| **LIR preset playback fix** (v0.8.20)                               | working — clicking *Play* on a Local-Internet-Radio (LIR) preset slot now starts the stream via `play-source` / the native ORION adapter instead of emulating a hardware `/key` press, which from a cold/idle speaker could leave the slot silent (#15) |
| **IP-Failsafe** — auto-remigrate on ESP-IP change, with pre-probe    | working — every migrated speaker stores the SixBack base URL as a fixed IP, so a DHCP change would strand them; SixBack detects its own IP change **at boot and at runtime** (WiFi reconnect event, v0.8.13) and re-points every speaker it owns, skipping those already on the new base. If a speaker is offline during the run (router swap — speakers boot slower than the ESP), the run retries every 60 s for up to 20 min instead of giving up (v0.8.13). A DHCP reservation for the SixBack MAC avoids the situation entirely and is still the recommended setup |
| **SETTLING status** (v0.6.541)                                       | working — backend reports `settling` instead of `offline` when only Telnet:17000 is down but BMX:8090 still answers |
| **Speaker status reliability** (v0.8.22)                            | working — fixed a status-tile flicker where the reachability fall-back probe hit the ESP's own port instead of the speaker's BMX port (8090) and flagged a live speaker as *offline*; the probe now targets the right port, uses a longer connect/read timeout, and a **2-strike debounce** requires two consecutive failed probes before a card flips to offline |
| Preset UI — drag&drop, dual-row (HW vs Store), per-slot revert       | working — modal progress, per-speaker export/import, refresh discards unsaved (v0.7.3)                             |
| **Custom stream library — device-side** (v0.8.5)                     | working — Stream-tab tiles persist in device NVS instead of per-browser localStorage; `GET/POST/DELETE /api/streams` + bulk import, one-time localStorage→device migration, Export/Import; survives USB-erase and browser change |
| **Add unreachable / LAN stream URLs** (v0.8.19)                      | working — the 🔗 Stream tab offers an *Add anyway* path: if the validator can't reach a URL (typical for LAN-only or self-hosted streams), the tile can still be saved, with a warning that the stream was not verified (#15) |
| **Speaker reordering** (v0.8.6)                                      | working — drag the ⠿ grip on a speaker card header to reorder the list; order is stored device-side (`POST /api/speakers/order`, persisted in NVS in the speaker-vector order), so it's identical in every browser and survives reboot; newly discovered speakers append at the end |
| Diagnostic snapshot (v0.6.0)                                         | working — `GET /api/speaker/{id}/diagnostic-snapshot` + one-shot pre-migrate snapshot persisted to `/snapshots/{deviceId}.json`; WebUI download or "Send to maintainer" upload to `sixback.io/snapshots/bosefix/snapshot` |
| OTA — app & LittleFS                                                 | working — `UPDATE_SIZE_UNKNOWN` + stream-to-EOF + 90% sanity-abort (v0.7.0 fix for HTTPS Content-Length truncation); a **size-scaled stall backstop** aborts a transfer that stops making progress, scaled to the image size instead of a fixed timeout (v0.8.22) |
| **OTA install — self-validating + clear status** (v0.8.3)            | working — the *Install* action re-checks the manifest itself instead of gating on a stale prior check, so a legitimate update is never blocked by a misleading "no update available"; distinct messages for *server unreachable* (retry) vs *already up-to-date* (use Force re-install); the WebUI panel always reflects the real state, so an error can no longer sit next to a stale "available" |
| **Manual "Flash web UI" — full-size S3 image** (v0.8.4)              | working — the WebUI upload guard rejected the ~9.9 MB S3 LittleFS image against a leftover 1 MB cap; raised to 11 MB so a manual FS upload matches the S3 spiffs partition. Verified end-to-end on S3 test hardware (~9.9 MB upload written, rebooted, FS intact). Also in v0.8.4: larger at-a-glance speaker status dots, and a GitHub project link in the WebUI + landing-page footer |
| **Tag-based release versioning** (v0.7.6)                            | working — `RELEASE_TAG` env bakes the same version string into all four target firmwares; eliminates multi-target build-drift |
| **Build size-gate** (v0.7.5)                                         | working — `build_release.sh` aborts if any firmware or LittleFS image exceeds its partition slot                   |
| **A/B-OTA partition layout**                                         | working — C3 / C6 / classic use symmetric `partitions-4mb.csv`: two 1.90 MB app slots (app0/app1) + 256 KB spiffs, so OTA flips between slots (no USB needed for updates). S3 uses two 3 MB app slots + 10 MB spiffs (`partitions.csv`). The size-gate refuses any image that won't fit its slot |
| WiFi provisioning — Improv-Serial (idle-window) + Captive AP         | working — both armed in parallel on cold boot                                                                      |
| **ESP32-C6 WPA2 reliability**                                        | working — `WiFi.setSleep(WIFI_PS_NONE)` + `setAutoReconnect(true)` applied **before** `WiFi.begin()`; closes 4-Way-Handshake-Timeout on WPA2-Mixed APs |
| System health — Task-WDT, WiFi / heap watchdog, crash counter, self-ping | working                                                                                                        |
| **Discovery stack-safety** (v0.8.5)                                  | working — the background discovery worker no longer overruns its task stack on setups with many speakers: SSDP responder collection and per-speaker probing now run in separate stack frames and the worker stack was enlarged. Fixes a stack-canary crash that rebooted the device mid-scan and left discovery finding 0 speakers (manual add still worked). Verified across S3 / C6 / C3 |
| **ESP32-C5 dual-band target** (v0.8.28, **not recommended as of v0.8.32**) | works, with a caveat — `c5` build for the ESP32-C5 (RISC-V, dual-band Wi-Fi 6). Verified end-to-end on real C5 silicon (rev v1.0): boots from the C5's `0x2000` bootloader offset, connects on **5 GHz** (channel 40), Web UI + provisioning + OTA-check all work. 4 MB / no-PSRAM devkit → C6-equivalent config; `/api/status` reports `band` + `channel`. Factory-image merge uses esptool ≥ 5 (4.x has no esp32c5 target). **Caveat:** the C5 has the smallest internal SRAM of all targets (384 KB → ~258 KB heap). Under heavy Web-UI load its heap can be exhausted to the point where the Wi-Fi stack cannot allocate and the device goes **unreachable without rebooting** until the load stops. Measured under an identical synthetic flood: C5 blacked out (min. free heap ~6 KB), while the **C6** (512 KB SRAM, ~394 KB heap, min. free 154 KB) and the **S3** (PSRAM) never lost a single ping — so this is a memory-capacity limit, not a core-count one (the C6 is single-core too). v0.8.32 reduces the peak but does not remove the limit on the C5 — and shipped a regression that rejected nearly all per-speaker preset reads on the C5 ("device busy (low heap)"), fixed in v0.8.33. v0.8.34 closes the practical trigger (two concurrent Web-UI page loads) with a per-transfer concurrency guard and lowers the read-guard floor to match real C5 heap levels; extreme request floods can still exhaust the board. Use **S3 or C6** for new builds; the C5 target stays in the tree and is community / PR-driven |
| Builds for **ESP32-S3 ★ / ESP32-C5 / ESP32-C3 / ESP32-C6 / ESP32-classic** | working — S3 is the recommended target; ESP32-classic re-enabled (`scripts/fs_exclude_esp32.py` trims the Spotify-only `silence.mp3` from its LittleFS image so the Web UI fits the 256 KB spiffs slot of `partitions-4mb.csv`) |
| ESP-Web-Tools landing page (auto-detects chip)                       | working — <https://sixback.io/>                                                                                    |

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
| ESP32-C5      | `esp32-c5-devkitc1-n4`          | 4 MB  | **dual-band Wi-Fi 6 (2.4 + 5 GHz)** — native USB-Serial-JTAG; verified connecting on 5 GHz (channel 40; `band`/`channel` shown in `/api/status`). 4 MB / no-PSRAM devkit, A/B-OTA like C3/C6. **Note:** the C5 second-stage bootloader lives at flash `0x2000` (not `0x0`), and merging its factory image needs esptool ≥ 5. 8 MB+PSRAM C5 boards (e.g. Seeed XIAO ESP32-C5) would warrant a separate build target |

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

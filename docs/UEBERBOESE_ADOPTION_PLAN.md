# Adoption-Plan: Was übernehmen wir aus julius-d/ueberboese-api?

**Stand:** 2026-05-19
**Referenz-Repo:** https://github.com/julius-d/ueberboese-api (Java/Spring Boot,
MIT, ~30 Klassen, 34 Stars)
**Eigenes Repo:** BoseFix32 (ESP32-S3, dieser Tree)

Quelle dieser Bewertung: Code-Review der `ueberboese-api.yaml`,
`BmxService.java`, `set-up-this-speaker.sh`-Template, Migrations- und
DB-Schema-Files (V001–V009).

## Worauf der Plan nicht eingeht

- **Server-Architektur** (Docker + zwei DNS-Subdomains + Java + H2): genau das,
  was BoseFix32 vermeidet. Skala ist anders, Zielgruppe ist anders.
- **Spotify-OAuth-Subdomain**: overkill für unseren aktuellen Scope.
- **Companion-Android-App**: OCT-Web-UI deckt das ab.
- **Proxy-Fallback an echte Bose-Server**: post-2026-05-06 tot.

## Übernahme — priorisiert

### P0 — Endpoint-Coverage-Audit (½ Tag)

**Ziel:** stilles 404-Inventar. Wir wissen nicht zuverlässig, welche Endpoints
Speaker in der Praxis rufen und wir noch nicht beantworten.

**Vorgehen:**
1. `ueberboese-api.yaml` (25+ Endpoints) gegen unsere Handler-Tabelle in
   `bose_endpoints.cpp` diffen.
2. Live-Log 24 h: alle "unhandled path"-Lines extrahieren. Voraussetzung: P3.
3. Resultat als `docs/ENDPOINT_COVERAGE.md` mit Spalten
   `[path, method, frequency, ueberboese-handles, bosefix-handles, status]`.

**Blockiert von:** P3.

### P1 — Stub-Endpoints für stille 404's (1 Tag)

**Ziel:** Speaker hört auf, vergeblich zu retryen. Logs werden leiser.
Eventuell funktionieren Sub-Features die wir nicht als kaputt wahrnehmen.

Aus `ueberboese-api.yaml` als trivialer 200 stubbar:

| Endpoint | Method | Body | Status BoseFix32 |
|---|---|---|---|
| `/streaming/support/power_on` | POST | leer | bereits da |
| `/streaming/support/customersupport` | GET | leerer XML-Container | fehlt |
| `/streaming/account/{a}/provider_settings` | GET | leer 200 | bereits da |
| `/streaming/device/{d}/streaming_token` | GET | Header `Authorization: <mock>` | fehlt |
| `/streaming/software/update/account/{a}` | GET | `softwareUpdateLocation=""` | fehlt |
| `/v1/blacklist/{deviceId}` | POST | leer 200, **Body 24h sammeln zuerst** | unklar |
| `/v1/scmudc/{deviceId}` | POST | leer 200 (jetzt), **Body loggen statt verwerfen** | bereits da, aber Body verworfen |

**Blockiert von:** P0.

### P3 — Catch-All-Logger (½ Tag) — vorgezogen

**Ziel:** P0-Audit überhaupt möglich machen. Bisher loggen wir nur registrierte
Handler; unhandled paths gehen nach `handleNotFound` ohne Pfad-Aufzeichnung.

**Vorgehen:**
1. `handleNotFound` in `bose_endpoints.cpp` (Line 215) erweitern:
   - Path, Method, Query-String, Content-Length zum `Serial.printf` loggen.
   - Body (falls vorhanden): erste 512 Bytes via AsyncWebServer-Body-Handler.
2. Ringpuffer im RAM (50 Einträge): `{ts, ip, method, path, body_snippet}`.
3. Neuer `/api/unknown-requests`-Endpoint im UI-Server für Browser-View.
4. UI: kleiner Tab oder Accordion "Unknown Requests" im Web-UI.

**Warum vor P0 ziehen:** P0 ohne P3 ist Lesen-im-Quelltext-statt-Beobachten —
Speaker tun in der Praxis Dinge, die in der `.yaml` nicht stehen.

### P2 — Event-Capture + "Now Playing"-UI (1 Woche)

**Ziel:** sichtbares neues Feature. Speaker POSTen `play-state-changed`,
`item-started`, `art-changed`, `volume-change`, `preset-pressed`,
`power-pressed`, etc. (ueberboese-Statistik: 4479 play-state-changed in einer
Capture-Periode).

**Vorgehen:**
1. **Endpoint:** `POST /v1/events/{deviceId}` (Pfad via P3-Logger verifizieren).
2. **Ringpuffer pro Device**, RAM only: 50 Events × ~5 Devices = ≤ 250 Records.
3. **Web-UI:** zweiter Abschnitt in der Speaker-Karte:
   "Aktuell: {title} — {artist} ({source})". Polling 2–5 s.
4. **Limit:** hartcoded 50 Events/Device; älteste fliegen raus.

**Risiko:** Event-Volumen hoch. Strategie: nur `play-state-changed` + 
`item-started` + `art-changed` parsen für UI; Rest dropen oder nur loggen.

### P4 — OverrideSdkPrivateCfg.xml Direct-Write (1 Tag Test + 2 Tage Impl)

**Ziel:** sauberer Rollback-Pfad ("Datei löschen → Factory-Cloud-Defaults") und
Fallback wenn Diag-Shell-Pfad mal nicht greift.

**Daten:** ueberboese's `set-up-this-speaker.sh` schreibt
`/var/lib/Bose/PersistenceDataRoot/OverrideSdkPrivateCfg.xml` direkt mit:

```xml
<SoundTouchSdkPrivateCfg>
    <margeServerUrl>http://server:port</margeServerUrl>
    <statsServerUrl>http://server:port</statsServerUrl>
    <swUpdateUrl>http://server:port/updates/soundtouch</swUpdateUrl>
    <isZeroconfEnabled>true</isZeroconfEnabled>
    <usePandoraProductionServer>true</usePandoraProductionServer>
    <saveMargeCustomerReport>false</saveMargeCustomerReport>
    <bmxRegistryUrl>http://server:port/bmx/registry/v1/services</bmxRegistryUrl>
</SoundTouchSdkPrivateCfg>
```

**Vorgehen — Test ZUERST (nicht Emma, sondern Greta oder Küche):**
1. Telnet 17000 → `remote_services on` → Root-Shell.
2. Datei manuell schreiben + Reboot. Verhalten gegen Diag-Shell-Migration
   vergleichen.
3. Datei löschen + Reboot. Erwartet: Factory-Cloud-Defaults.

**Falls Test grün:** Als 2. Migrations-Pfad in BoseFix32 implementieren,
Default bleibt Diag-Shell. Advanced-Tab-Toggle.

**Falls Test rot** (Speaker ignoriert File ohne Diag-Shell-Trigger): in
`RESEARCH.md` festhalten, Pfad fallenlassen.

**Status:** Conjecture bis verifiziert. Ihre Methode schreibt im Shell-Kontext;
ob die Datei standalone vom Speaker-Init geladen wird, ist nicht bewiesen.

### P5 — Stereo-Pairing für ST10 (1–2 Wochen Research-Sprint)

**Ziel:** Feature das Bose explizit als Verlust angekündigt hat. Hoher
Wahrnehmungs-Wert.

**Vorgehen:**
1. Endpoint-Spec lesen: `/streaming/account/{a}/device/{d}/group/` (POST = create)
   + `/streaming/account/{a}/group/{g}` (PATCH/DELETE).
2. DB-Modell von ueberboese: `master_device_id`, `left_device_id`,
   `right_device_id` — für ESP32 NVS reicht 3× device_id + Name.
3. **Mit 2× ST10 testen.** Pair erstellen, prüfen ob Audio tatsächlich synchron
   spielt.
4. **Implementieren** wenn Test grün.

**Risiko hoch:** Wenn der Cloud-Server aktiv im Audio-Sync-Pfad steckt (nicht
nur Konfig vermittelt), Sackgasse. Empirisch klären.

## Verschoben / nicht jetzt

- **Custom-Stations base64-Trick** (eleganter als unsere RadioBrowser-Pipeline
  aktuell für ad-hoc Stream-URLs ist, aber kein akutes Defizit).
- **Proxy-Forward an Bose**: Server tot — nur Logger-Variante (P3) hat Wert.
- **Spotify-OAuth**: separates Großprojekt, wenn dann via OCT-Sidecar.

## Reihenfolge konkret

```
Tag 1:  P3  (Catch-All-Logger)         → Voraussetzung für alles
Tag 2:  P0  (Coverage-Audit)
Tag 3:  P1  (Stub-Endpoints)
Tag 4:  P4 Test (Direct-File-Write)    → parallel zu P2 möglich
Tag 5+: P2  (Event-Capture + UI)
        P4 Implementation (falls Test grün)

Sprint 2: P5 (Stereo-Pairing)
```

**Gates:**
- P3 muss vor P0 laufen (sonst auditen wir blind).
- P0-Tabelle muss vor P1 stehen (sonst stubben wir falsche Endpoints).
- P4-Test muss vor P4-Implementation stehen.
- P5 nicht starten bevor P2 + P4 fertig sind.

## Was wir aus ueberboese-api bestätigt sehen

- **Migration über Diag-Shell Port 17000** ist der etablierte Standard-Pfad.
- **HTTP only, kein TLS** auf der Speaker-Cloud-URL — bestätigt unsere
  TLS-Pinning-Sackgasse-Beobachtung.
- **TuneIn OPML-API** (`describeUrl` + `streamUrl` mit `s80044`-IDs) ist der
  richtige Resolver-Pfad — wir machen das schon so.

## Adoption-Stand 2026-05-19

| Phase | Status | Anmerkung |
|---|---|---|
| P0 Coverage-Audit | ✅ done | erste 5-min Capture: 24 Reqs, 3 Buckets (sourceproviders, /, bmx-icons) |
| P3 Catch-All-Logger | ✅ done | Ringbuffer + `/api/unknown-requests` |
| P1 Stub-Endpoints | ✅ done | nach P1: 0 unknown reqs in 4 min |
| P2 Event-Capture | ⚠ Backend-ready, Stream blockiert | siehe Addendum unten |
| P6 Account-Response anreichern | 📋 neu | Vorbedingung für P2-Realitäts-Test |
| P4 OverrideSdkPrivateCfg.xml | pending | ggf. interessanter nach P6-Erkenntnissen |
| P5 Stereo-Pairing | pending | nach P2+P4 |

## P2-Addendum (Diagnose 2026-05-19)

P2-Implementation (handleSCMUDC mit Body-Capture, event_store, UI-Endpoints
`/api/events`, `/api/speaker/{id}/now-playing`, `/events`) ist im Tree und
funktional verifiziert per Synthetic-POST.

**Aber:** Speaker schicken in unserer aktuellen Konfiguration **keine**
scmudc-Posts. Diagnose-Pfad:

| Beweis | Bedeutung |
|---|---|
| `chunks_seen=0` nach aktivem `/select TUNEIN` an Küche | Body-Handler nie aufgerufen |
| `unknown_requests=0` während desselben Tests | Auch keine anderen Pfade gerufen |
| Synthetic-POST → korrektes NowPlaying im Store | Pipeline ist OK |
| Telnet `getpdo CurrentSystemConfiguration` → `statsServerUrl=http://10.10.11.169:8000` | URL ist korrekt |
| Greta+Emma `/select TUNEIN` → `UNKNOWN_SOURCE_ERROR` | Cloud-Sources halb geladen |
| Küche `/select TUNEIN` → 200 | Hat noch Pre-Migration-Bose-Cache (2 TUNEIN-Einträge) |

**Wurzel-Vermutung:** Unsere `/streaming/account/{a}/full` ist zu mager. Wir
liefern:

```xml
<account><accountID>anonymous</accountID><sources>...</sources><presets/></account>
```

Ueberboese liefert (siehe `src/test/resources/test-data/streaming-account-full-6921042.xml`):

```xml
<account id="6921042">
  <accountStatus>CHANGE_PASSWORD</accountStatus>
  <devices>
    <device deviceid="123980WER">
      <attachedProduct product_code="SoundTouch 10 sm2"><productlabel>soundtouch_10</productlabel></attachedProduct>
      <firmwareVersion>27.0.6.46330...</firmwareVersion>
      <ipaddress>192.168.178.2</ipaddress>
      <name>Foobar</name>
      <presets>
        <preset buttonNumber="1">
          <containerArt>http://...</containerArt>
          <contentItemType>stationurl</contentItemType>
          <location>/v1/playback/station/s80044</location>
          <source id="19989342" type="Audio">
            <credential type="token">eyJduTune=</credential>
            <sourceproviderid>25</sourceproviderid>
          </source>
        </preset>
      </presets>
    </device>
  </devices>
</account>
```

Mit voller Account-Response erwarten wir gleichzeitig:
1. scmudc-Event-Stream startet → P2 testbar
2. Greta+Emma `/select TUNEIN` funktioniert wieder (echte credential)
3. Preset-Push-To-Device an Greta funktioniert

→ P6 in der Reihenfolge.

---

## Sweep-Stand 2026-05-19 abends (post P2+P4+P6-Resolution)

**Phase-Status:**

| Phase | Status | Commit |
|---|---|---|
| P0 Endpoint-Coverage-Audit | done | — |
| P1 Stub fehlende stille 404 (sourceproviders, /, bmx-icons) | done | `6b9ab3e` |
| P3 Catch-All-Logger im handleNotFound | done | `6b9ab3e` |
| P2 Event-Capture / NowPlaying / scmudc | **done** | `935e817`+`ef6c080`+`5974f5f` |
| P6 Account-Response anreichern | **done** | `0e10d40` |
| P4 OverrideSdkPrivateCfg.xml direct-write | **OBSOLET** (API-Pfad via /setMargeAccount funktioniert) | — |
| P5 Stereo-Pairing/Multi-Room-Group-Endpoints | **done 2026-05-19** (POST/PUT/DELETE + NVS-persistent + Hardware-verifiziert) | `85dffa1` |
| P7 Defensive Stubs (orion/station, sw-update, streaming_token, customersupport, recents, recent/n, preset/n) | **done 2026-05-19** (alle 7 smoke-getestet) | `6b7d15e` |

**Endpoint-Coverage:** 22/30 ueberboese-Endpoints bedient (post P7) + 6
extra empirisch-beobachtete (root, bmx-icons, sources-kurz, presets-alt,
updates/soundtouch). 8 verbleibende = App-only-CRUD (PUT/DELETE auf
device/preset, OAuth) — bewusst nicht implementiert, siehe unten.

**Schema-Pattern (lehrreiche Anmerkungen aus ueberboese-Spec):**

1. **scmudc-Body-Format ist JSON** (nicht XML wie der Rest):
   ```
   { envelope:{...}, payload:{ deviceInfo:{...}, events:[{type,data,...}] } }
   ```
2. **Bose-nowPlaying-Struct-Felder verschachtelt mit `.text`-Subkey:**
   ```
   "track":  { "text": "..." }
   "artist": { "text": "..." }
   "art":    { "artImageStatus": "IMAGE_PRESENT", "text": "url" }
   ```
3. **Event-Type-Felder sind kebab-case**, nicht camelCase:
   - `data.play-state` (nicht `playState`)
   - `data.art-uri` (nicht `artUri` oder `art_url`)
   - `data.source-state` (nicht `sourceState`)
   - `data.volume-change` ist ein **Array** `[v0..vFinal]`
   - `data.system-state` ist Mixed-Case: `"On"`/`"Standby"` (nicht UPPER)
4. **Pair-Bootstrap-Endpoint** (trailing-slash, sonst 404):
   ```
   POST /streaming/account/{aid}/device/
        Content-Type: application/vnd.bose.streaming-v1.2+xml
        Body: <device deviceid="XXX"><name>...</name><macaddress>...</macaddress></device>
   ```
   Response **muss** sein:
   ```
   HTTP/1.1 201 Created
   Credentials: Bearer <synth-token>
   Location: http://<server>/streaming/account/<aid>/device/<did>
   METHOD_NAME: addDevice
   <device deviceid="<did>">...</device>
   ```
   **Critical:** deviceid im Response leer → Speaker gibt 500. Body muss
   deviceid aus dem Request zurueckspiegeln.
5. **Speaker-Side Endpoint `/setMargeAccount` (Port 8090):**
   ```
   POST <PairDeviceWithAccount><accountId>X</accountId><userAuthToken>Bearer Y</userAuthToken></PairDeviceWithAccount>
   ```
   Triggert intern den /streaming/account/{aid}/device/ Roundtrip.
6. **`/select` ContentItem braucht `sourceAccount` der zum Speaker-/sources-Eintrag passt:**
   TUNEIN → `sourceAccount="TuneIn"` (nicht leer). Sonst HTTP 500
   UNKNOWN_SOURCE_ERROR.

## P7 abgeschlossen 2026-05-19 (Commit `6b7d15e`)

Alle 7 Speaker-aktive Gap-Filler-Endpoints sind defensiv gestubbt:

| Endpoint | Status | Antwort |
|---|---|---|
| `GET /core02/svc-bmx-adapter-orion/.../station` | done | 200 JSON `{streamUrl:"",imageUrl:"",name:""}` |
| `GET /streaming/software/update/account/{aid}` | done | 200 XML `<software_update><softwareUpdateLocation/></software_update>` |
| `GET /streaming/device/{did}/streaming_token` | done | 200 empty + `Authorization` Header |
| `POST /streaming/support/customersupport` | done | 200 empty (Body wird Serial-geloggt) |
| `GET /streaming/account/{a}/device/{d}/recents` | done | 200 XML `<recents/>` |
| `GET /streaming/account/{a}/device/{d}/recent/{rid}` | done | 404 XML `<status>...</status>` |
| `GET /streaming/account/{a}/device/{d}/preset/{n}` | done | 404 XML `<status>...</status>` |

Status-Check: Greta+Emma+Kueche `unknown-requests count=0` weiterhin —
keiner der Stubs wird derzeit aktiv abgefragt, aber bei FW-Update oder
Speaker-Mode-Wechsel ist jetzt jederzeit eine Spec-konforme Antwort da.

## App-only-Endpoints (NICHT implementieren)

Phone-App-CRUD-Endpoints werden von BoseFix32 nicht benoetigt — unsere
UI macht das selber (push-to-device, /api/speakers/{id}/migrate, etc.):
PUT/DELETE auf device/preset/group, OAuth-Token-Refresh.

P5 (Stereo-Pairing/Multi-Room) ist seit 2026-05-19 implementiert (POST/PUT/
DELETE auf `/streaming/account/{a}/group/`, NVS-persistent). Die Phone-App-
Variante koennte das jetzt direkt nutzen falls die App je wieder ins Spiel
kommt — fuer den lokalen Bedarf wuerde aber zusaetzlich ein UI-Hook noetig
sein (BoseFix32-Web-UI ruft die Endpoints selber auf).

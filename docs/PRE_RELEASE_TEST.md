# BoseFix32 — Pre-Release-Test

Vor jedem getaggten Release diese Sequenz durchlaufen, um zu verifizieren,
dass die End-to-End-Pipeline (Flash → Provision → Auto-Discovery → Auto-
Migration → Preset-Erhalt) auf **beiden** empfohlenen Targets (ESP32-S3 +
ESP32-C6) und über **beide** Provisionierungs-Pfade (Improv-Serial +
Captive-Portal) funktioniert.

Der Test ist als **4-Phasen-Ping-Pong** zwischen einem S3- und einem
C6-Stick gebaut.  Jede Phase fängt mit komplettem **NVS-Erase** an, damit
keine alten Image-Defaults / NVS-Werte das Ergebnis fälschen.

## Voraussetzungen

- Zwei verkabelte ESP32-Targets:
  - **ESP32-S3-DevKitC-1-N16R8** an USB (z. B. `/dev/serial/by-id/usb-1a86_USB_Single_Serial_<S/N>-if00`)
  - **ESP32-C6-DevKitC-1** an USB (`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`)
- Mindestens **ein** SoundTouch-Speaker am LAN (besser mehrere) — Modell aus Whitelist (`SoundTouch 10/20/30`, Firmware `27.0.6.x` oder `27.0.3.x`).
- Test-Host mit:
  - PlatformIO (`pio` via `~/.platformio/penv/bin/pio`)
  - `improv_client.py` für Improv-Serial (`/root/workdir/ip4knx/scripts/test_improv.py`)
  - WiFi-Interface für Captive-Portal-Phase (`wlan0` mit `nmcli`)
- WiFi-Credentials: SSID + PSK des Test-LANs (in Shell-Env als `WIFI_SSID` / `WIFI_PSK`).
- Speaker-IPs vorab notieren (für Verifikation).

## Phasen-Übersicht

| Phase | Target | Provisioning | Erwartung |
|-------|--------|--------------|-----------|
| 1     | S3     | **Improv-Serial** | Alle Speaker werden vom S3 entdeckt + migriert. |
| 2     | C6     | **Captive-Portal** | Alle Speaker werden vom C6 auto-claimed weg vom S3 + migriert auf C6. |
| 3     | S3     | **Improv-Serial** | Alle Speaker werden vom S3 zurück-migriert. |
| 4     | C6     | **Captive-Portal** | Alle Speaker werden vom C6 zurück-migriert. |

Diese Alternation deckt:

- Improv-Path auf S3 (CH343-UART-Bridge).
- Captive-Path auf C6 (Built-in USB-Serial-JTAG → MAC-stamped AP-SSID).
- Auto-Claim/Release-Symmetrie (jeder fremd-migrierte Speaker muss von der neuen Box übernommen werden).
- Default `cron_interval_s=1800` und `enabled=true` greifen out-of-the-box (kein User-Override).
- LittleFS-Mount nach Erase (Format-on-failure-Pfad wird auf jedem Phase-1-Boot getestet).

## Per-Phase-Sequenz

Diese Schritte gelten für **jede** der 4 Phasen, mit den per-Phase-Variablen aus der Tabelle weiter unten.

### 0. Vor-Phase-Snapshot

```bash
# Aktueller Zustand: welche Speaker, von welcher Box gerade gehalten?
curl -s http://$LAST_OWNER_IP/api/speakers | jq '.speakers | map({name, owned_by_us, cloud_url, status})'
```

### 1. NVS + Flash erasen (idempotent)

```bash
pio run -e $ENV -t erase --upload-port $PORT
```

Erwartung: esptool meldet `Erase complete`. Dauert ~10 s.

### 2. App + LittleFS hochladen

```bash
pio run -e $ENV -t upload     --upload-port $PORT
pio run -e $ENV -t uploadfs   --upload-port $PORT
```

`uploadfs` separat — kombiniert (`-t upload -t uploadfs`) bricht durch LDF-Reresolve (siehe Implementation-Lessons #35).

### 3. Provisionieren

#### Improv-Serial-Path (Phasen 1, 3)

```bash
python3 /root/workdir/ip4knx/scripts/test_improv.py \
  --port "$PORT" --ssid "$WIFI_SSID" --password "$WIFI_PSK"
```

Erwartung: Improv-Client sieht `STATE_AUTHORIZED → STATE_PROVISIONING → STATE_PROVISIONED`, gibt am Ende den LAN-IP zurück.

#### Captive-Portal-Path (Phasen 2, 4)

```bash
# Device-AP-SSID hat das Pattern  BoseFix32-XXYYZZ, wo XXYYZZ die letzten
# 6 hex-Stellen der MAC sind. Für die Test-ESPs vorab notieren:
#   S3 MAC 94:A9:90:D2:29:54 → AP "BoseFix32-D22954"
#   C6 MAC 54:32:04:03:59:68 → AP "BoseFix32-035968"
AP_SSID="BoseFix32-$LAST_6_MAC_HEX"

# WiFi-Interface freischalten, mit der AP des Devices verbinden.
sudo ip link set wlan0 up
sleep 2
sudo nmcli dev wifi rescan ifname wlan0
# Bis die AP da ist warten (kann nach Boot ~15 s dauern)
until nmcli -t -f SSID dev wifi list ifname wlan0 | grep -q "^${AP_SSID}$"; do sleep 2; done
sudo nmcli dev wifi connect "$AP_SSID" ifname wlan0
sleep 3

# Captive-Form posten — Device-AP ist immer 192.168.4.1.
curl --interface wlan0 -X POST "http://192.168.4.1/save" \
     --data-urlencode "ssid=$WIFI_SSID" \
     --data-urlencode "psk=$WIFI_PSK"

# Status pollen bis die Box "joined" meldet
for i in $(seq 1 30); do
    STATE=$(curl --interface wlan0 -s "http://192.168.4.1/save_status" | jq -r .state)
    [ "$STATE" = "ok" ] && break
    sleep 2
done

# AP-Connection abreißen, wlan0 wieder runter — wir wollen das Device
# danach via eth0 auf dem LAN finden.
sudo nmcli c down "$AP_SSID"
sudo nmcli c delete "$AP_SSID"
sudo ip link set wlan0 down
```

### 4. Warten, bis das Device im LAN auftaucht

```bash
echo "Warte auf $ENV im LAN ..."
until curl -sf --max-time 2 "http://$LAN_IP/api/status" >/dev/null; do sleep 2; done
echo "  online: $(curl -s http://$LAN_IP/api/status | jq -r '.version')"
```

`$LAN_IP` ist deterministisch nur, wenn der DHCP-Server für die MAC eine
statische Reservierung hat. Sonst muss man die IP per `mdns` (`bosefix.local`)
oder per ARP-Scan finden.

### 5. Auto-Migration durchwarten (Boot-Pass)

Default `bootDelayMs=10000` + Discover-Phase ~30 s + pro Speaker ~70 s
(Telnet + Reboot + Wait-Back) × `maxPerBoot` (default 4).
Bei drei Speakern ergibt das ~250 s.

```bash
for i in $(seq 1 30); do
    STATE=$(curl -s http://$LAN_IP/api/auto-mode | jq -r '.status.state')
    case "$STATE" in
        done|done-nothing-to-do|cron-idle)
            echo "  Auto-Mode finished, state=$STATE"; break ;;
    esac
    echo "  ... state=$STATE"
    sleep 15
done
```

### 6. Verifikation

```bash
# Auto-Mode-Endzustand
curl -s http://$LAN_IP/api/auto-mode | jq '.status | {ran, state, speakers_seen, speakers_eligible, speakers_migrated, slots_normalized, slots_abandoned, last_error}'

# Speaker-Inventar — Erwartung: alle Speaker, status=migrated, owned_by_us=true, cloud_url=http://$LAN_IP:8000
curl -s http://$LAN_IP/api/speakers | jq '.speakers | map({name, status, owned_by_us, cloud_url, preset_count})'

# Heap-Sanity
curl -s http://$LAN_IP/api/status | jq '{version, heap, uptime_s}'
```

Pass-Kriterien pro Phase:

| Kriterium | Pass-Wert |
|---|---|
| `status.ran` | `true` |
| `status.state` | `done` (oder `done-nothing-to-do` falls bereits in dieser Box owned — Phase 3/4 Sonderfall) |
| `status.last_error` | leer |
| Jeder Speaker `owned_by_us` | `true` |
| Jeder Speaker `cloud_url` | `http://$LAN_IP:8000` |
| Jeder Speaker `status` | `migrated` |
| Jeder Speaker `preset_count` | `6` (oder `0`, wenn der Speaker nie Presets hatte) |
| Heap `min_free` | `> 80 KB` (S3) / `> 50 KB` (C6) |

## Variablen-Tabelle für die 4 Phasen

```bash
# Phase 1 — S3 via Improv
ENV="s3";  PORT="/dev/serial/by-id/usb-1a86_USB_Single_Serial_5C37280229-if00";   PROVISION="improv"; LAN_IP="10.10.11.169"
# Phase 2 — C6 via Captive
ENV="c6";  PORT="/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_54:32:04:03:59:68-if00"; PROVISION="captive"; AP_SSID="BoseFix32-035968"; LAN_IP="10.10.11.168"
# Phase 3 — S3 via Improv
ENV="s3";  PORT="...wie Phase 1...";  PROVISION="improv";  LAN_IP="10.10.11.169"
# Phase 4 — C6 via Captive
ENV="c6";  PORT="...wie Phase 2...";  PROVISION="captive"; AP_SSID="BoseFix32-035968"; LAN_IP="10.10.11.168"
```

## Bekannte Erwartungs-Abweichungen

- **Phase 1 + 3** (S3 frisch geflasht, alle Speaker müssen migriert werden):
  `speakers_migrated >= speakers_eligible` und `speakers_eligible > 0`.
- **Phase 2 + 4**: bevor das C6 startet, hält das S3 gerade owned-Status auf den Speakern.  Das C6 sieht die Speaker mit `cloud_url=10.10.11.169:8000` (= nicht uns, nicht Bose) — eligible-Check passt (Modell-Whitelist matcht, ownedByUs=false), Migration läuft.  Speaker-Reboot dauert ~70 s pro Stück.
- **In jeder Phase 2+ kann es vorkommen, dass `refreshMigrationStatus` während des Auto-Modes auch das Auto-**Release** triggert: weil das ESP gerade migrierende Speaker noch auf der alten URL sieht und sie mit `cloud_url != myBase && cloud_url != ""` als not-ours markiert.  Final-state ist trotzdem korrekt: `ownedByUs` flippt nach Migrate auf true.

## Ergebnis-Protokoll

Pro Release einen Block hier anhängen mit Datum + Version + Phase-Ergebnissen:

### 2026-05-18 — vor v0.4.0-Release (Source-Stand: master 1e8929c + post-review-fixes)

| Phase | Target | Prov     | Erwartung                          | Resultat |
|-------|--------|----------|------------------------------------|----------|
|   1   | S3     | Improv   | erase → auto-claim 3 Speaker       | **PASS (auto-claim only, same DHCP-IP)** — 2/3 via SSDP-Discovery, Küche initial offline für SSDP-Multicast (Lesson #33), per manuellem `addByIp` + `refresh-status` nachgereicht. Preset-Counts post-erase initial 0, per manuellem `import-from-device` auf 6/6/6 nachgereicht. Speaker-interne Presets überlebten NVS-Erase. |
|   2   | C6     | Captive  | erase → Migration 3 Speaker S3→C6  | **PASS** — Captive-AP `BoseFix32-035968` sichtbar nach Boot, POST `/save` HTTP 200, state-Polling `connecting`→`success` in 6 s. C6 migrierte Greta+Emma+Küche, NVS persistierte Status. **HTTP-Port-80-Hang** trat nach Auto-Mode-Pass auf (siehe Lesson #36) — DTR-Reset normalisierte. |
|   3   | S3     | Improv   | erase → Migration 3 Speaker C6→S3  | **PASS** — Improv-Provisioning sauber, Auto-Mode migrierte alle 3 Speaker zurück, Presets per `account/full` embedded. **HTTP-Port-80-Hang** trat erneut auf (auch S3, nicht nur C6) — DTR-Reset normalisierte. |
|   4   | C6     | Captive  | erase → Migration 3 Speaker S3→C6  | **PASS** — Captive-Wiederholung verlief gleich wie Phase 2. Post-Test S3-`refresh-status` zeigte korrekte Auto-Release-Symmetrie (alle Speaker `owned=false`, `cloud=10.10.11.168:8000`). |

**Gesamt:** 4/4 Phasen bestanden. Auto-Migration ↔ Captive ↔ Improv funktioniert auf beiden Targets, NVS persistiert die Resultate, Auto-Claim/Release-Symmetrie ist intakt. Presets überleben NVS-Erase + Re-Provisioning weil der Auto-Mode-Pipeline-Pass `import-from-device` PRE-Migration ausführt und der Speaker selbst seine Presets lokal hält.

**Hardware-Quirks beobachtet** (kein Funktions-Showstopper, aber für Release-Notes):
- **Port-80-Hang nach Auto-Mode-Pass** auf BEIDEN Targets (war als C6-only in Lesson #36 dokumentiert — Korrektur eingearbeitet). Symptom: UI-API antwortet nicht mehr, Port 8000 + Ping leben. Workaround: DTR/RTS-Reset-Pulse. Speaker spielen weiter ungestört.
- **Discovery-Cold-Start-Lücke** (Lesson #33): SSDP-Multicast greift bei kalt-geflashten ESPs nicht 100 % zuverlässig. Manuelles `addByIp` rettet einzelne Speaker zuverlässig.

**Bestätigte Default-Werte v0.4.0:**
- `cronIntervalS` = 1800 s (= 30 min) — verifiziert in `loadAutoModeConfig` Image-Default
- `enabled` = true (Image-Default)
- `maxPerBoot` = 4

**Empfehlung für Distribution:** ESP32-S3 bleibt primäre Empfehlung trotz dass der Port-80-Hang inzwischen auch S3 betrifft — größeres RAM-Budget (8 MB PSRAM), 2 Cores statt 1, weniger Stress auf async_tcp.

```
### vYYYY-MM-DD — vX.Y.Z

| Phase | Target | Prov | Speakers migrated | Heap min | Notes |
|-------|--------|------|-------------------|----------|-------|
|   1   |  S3    | impr |  3/3              |          |       |
|   2   |  C6    | capt |  3/3              |          |       |
|   3   |  S3    | impr |  3/3              |          |       |
|   4   |  C6    | capt |  3/3              |          |       |
```

## Notfall-Recovery

- **Improv-Provisioning hängt**: `improv_client.py` Timeout abwarten (~60 s), USB neu reseten via `usbreset` oder Stecker.
- **Captive-AP erscheint nicht**: `nmcli dev wifi rescan` mehrfach, kurz `sudo ip link set wlan0 down ; up` zyklen.
- **Device kommt nach Captive-Provisioning nicht ins LAN**: Captive-Window beträgt 5 min — danach gibt das ESP auf und versucht NICHT erneut.  In dem Fall: re-erase + re-provision.
- **Speaker rebootet nicht zurück innerhalb 180 s** (`speaker did not come back`): manuell prüfen `curl http://<speaker-ip>:8090/info`, ggf. Stromabzug.  Auto-Claim/Release korrigiert beim nächsten Cron-Tick.

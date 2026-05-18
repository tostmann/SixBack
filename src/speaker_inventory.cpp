// SPDX-License-Identifier: GPL-3.0-or-later
#include "speaker_inventory.h"
#include "nvs_helper.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>

namespace bosefix {

namespace {

constexpr const char* NVS_NS  = "bosefix-inv";
constexpr const char* NVS_KEY = "speakers";

// Hilfs-Regex-frei: einfaches String-extract zwischen <tag> und </tag>
String xmlValue(const String& xml, const String& tag) {
    String open = "<" + tag + ">";
    String close = "</" + tag + ">";
    int a = xml.indexOf(open);
    if (a < 0) return "";
    a += open.length();
    int b = xml.indexOf(close, a);
    if (b < 0) return "";
    return xml.substring(a, b);
}

// Extrahiert Attribut-Wert aus z.B. <interface ssid="X" ...
String xmlAttr(const String& xml, const String& attr) {
    String key = attr + "=\"";
    int a = xml.indexOf(key);
    if (a < 0) return "";
    a += key.length();
    int b = xml.indexOf('"', a);
    if (b < 0) return "";
    return xml.substring(a, b);
}

// Mini-Telnet: schickt cmd, wartet auf "->" Prompt, liefert Reply zurueck.
bool telnetSend(WiFiClient& c, const String& cmd, String& reply, uint32_t timeoutMs = 3000) {
    c.print(cmd + "\n");
    reply = "";
    uint32_t deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        while (c.available()) {
            char ch = c.read();
            reply += ch;
            if (reply.endsWith("->")) return true;
        }
        delay(15);
    }
    return false;
}

} // anon

SpeakerInventory& SpeakerInventory::instance() {
    static SpeakerInventory s;
    return s;
}

void SpeakerInventory::initMutex_() {
    if (!mx_) mx_ = xSemaphoreCreateRecursiveMutex();
}

SpeakerInventory::LockGuard::LockGuard(SpeakerInventory& inv) : inv_(inv) {
    inv_.initMutex_();
    xSemaphoreTakeRecursive(inv_.mx_, portMAX_DELAY);
}

SpeakerInventory::LockGuard::~LockGuard() {
    xSemaphoreGiveRecursive(inv_.mx_);
}

const char* migrationStatusToStr(MigrationStatus s) {
    switch (s) {
        case MigrationStatus::NOT_MIGRATED:   return "not_migrated";
        case MigrationStatus::MIGRATED:       return "migrated";
        case MigrationStatus::OFFLINE:        return "offline";
        default:                              return "unknown";
    }
}

void SpeakerInventory::loadFromNVS() {
    LockGuard g(*this);
    JsonDocument doc;
    if (!nvsLoadJson(NVS_NS, NVS_KEY, doc)) {
        Serial.println("[inv] no NVS-state, starting fresh");
        return;
    }
    speakers_.clear();
    for (JsonObject o : doc["speakers"].as<JsonArray>()) {
        Speaker s;
        s.deviceId   = (const char*)o["deviceId"];
        s.name       = (const char*)o["name"];
        s.model      = (const char*)o["model"];
        s.firmware   = (const char*)o["firmware"];
        s.ip         = (const char*)o["ip"];
        s.accountId  = (const char*)o["accountId"];
        s.status     = (MigrationStatus)(uint8_t)o["status"];
        s.cloudUrl   = (const char*)(o["cloudUrl"] | "");
        s.ownedByUs  = o["ownedByUs"] | false;
        s.lastSeenMs = 0;  // resetten nach reboot
        s.groupId    = (const char*)(o["groupId"] | "");
        speakers_.push_back(s);
    }
    Serial.printf("[inv] loaded %u speakers from NVS\n",
                  (unsigned)speakers_.size());
}

void SpeakerInventory::saveToNVS() {
    LockGuard g(*this);
    JsonDocument doc;
    JsonArray arr = doc["speakers"].to<JsonArray>();
    for (auto& s : speakers_) {
        JsonObject o = arr.add<JsonObject>();
        o["deviceId"]  = s.deviceId;
        o["name"]      = s.name;
        o["model"]     = s.model;
        o["firmware"]  = s.firmware;
        o["ip"]        = s.ip;
        o["accountId"] = s.accountId;
        o["status"]    = (uint8_t)s.status;
        o["cloudUrl"]  = s.cloudUrl;
        o["ownedByUs"] = s.ownedByUs;
        o["groupId"]   = s.groupId;
    }
    nvsSaveJson(NVS_NS, NVS_KEY, doc);
}

void SpeakerInventory::mergeSpeaker_(const Speaker& s) {
    LockGuard g(*this);
    for (auto& existing : speakers_) {
        if (existing.deviceId == s.deviceId) {
            existing.name       = s.name;
            existing.model      = s.model;
            existing.firmware   = s.firmware;
            existing.ip         = s.ip;
            existing.accountId  = s.accountId;
            existing.lastSeenMs = millis();
            // status NICHT ueberschreiben - das macht refreshMigrationStatus
            return;
        }
    }
    Speaker s2 = s;
    s2.lastSeenMs = millis();
    speakers_.push_back(s2);
}

bool SpeakerInventory::probeIp_(const String& ip, Speaker& out,
                                 uint16_t connectMs, uint16_t readMs) {
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(connectMs);
    http.setTimeout(readMs);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/info";
    if (!http.begin(url)) return false;
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String xml = http.getString();
    http.end();
    // Validierung: muss ein Bose-/info-XML sein
    if (xml.indexOf("<info") < 0) return false;
    out.deviceId  = xmlAttr(xml, "deviceID");
    out.name      = xmlValue(xml, "name");
    out.model     = xmlValue(xml, "type");
    out.firmware  = xmlValue(xml, "softwareVersion");
    out.accountId = xmlValue(xml, "margeAccountUUID");
    out.ip        = ip;
    out.status    = MigrationStatus::UNKNOWN;
    return out.deviceId.length() > 0;
}

void SpeakerInventory::ssdpMSearch_() {
    WiFiUDP udp;
    if (!udp.begin(0)) return;
    const char* msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
    IPAddress mcast(239, 255, 255, 250);
    for (int i = 0; i < 3; ++i) {
        udp.beginPacket(mcast, 1900);
        udp.write((const uint8_t*)msg, strlen(msg));
        udp.endPacket();
        delay(150);
    }
    Serial.println("[inv][ssdp] M-SEARCH burst sent, listening 4s ...");
    uint32_t deadline = millis() + 4000;
    char buf[1024];
    while (millis() < deadline) {
        int n = udp.parsePacket();
        if (n <= 0) { delay(20); continue; }
        IPAddress src = udp.remoteIP();
        int len = udp.read(buf, sizeof(buf) - 1);
        if (len <= 0) continue;
        buf[len] = 0;
        String resp(buf);
        // Pruefen ob es ein SoundTouch ist (Server-Header oder Location-Pattern)
        if (resp.indexOf("Location: http://") < 0) continue;
        // Bose-Speaker liefern z.B. http://10.10.11.196:8091/XD/BO5EBO5E-...
        String srcIp = src.toString();
        Speaker s;
        if (probeIp_(srcIp, s)) {
            Serial.printf("[inv][ssdp] %s -> %s (%s)\n",
                          srcIp.c_str(), s.name.c_str(), s.model.c_str());
            mergeSpeaker_(s);
        }
    }
    udp.stop();
}

// Probe-Pass ueber alle in NVS hinterlegten Speaker-IPs. Viel schneller +
// zuverlaessiger als SSDP/Active-Scan, weil wir genau die Adressen kennen
// die normalerweise antworten. Faengt Speaker auf, deren SSDP-M-SEARCH-
// Response von der WiFi-Multicast-Konvertierung verschluckt wurde — was
// auf ESP32-C6 + WiFi-6-Routern haeufiger passiert als auf S3.
void SpeakerInventory::knownIpProbe_() {
    std::vector<Speaker> snapshot;
    { LockGuard g(*this); snapshot = speakers_; }  // Kopie unter Lock
    if (snapshot.empty()) return;
    Serial.printf("[inv][known] probing %u known IPs ...\n",
                  (unsigned)snapshot.size());
    int hits = 0;
    for (auto& s : snapshot) {
        if (s.ip.length() == 0) continue;
        Speaker probe;
        if (probeIp_(s.ip, probe)) {
            Serial.printf("[inv][known] %s -> %s (%s) still alive\n",
                          s.ip.c_str(), probe.name.c_str(), probe.model.c_str());
            mergeSpeaker_(probe);
            ++hits;
        }
    }
    Serial.printf("[inv][known] %d/%u known IPs responded\n",
                  hits, (unsigned)snapshot.size());
}

void SpeakerInventory::activeScan_() {
    // Subnetz aus eigener IP ableiten: nur fuer /24-Netze, andere skippen
    IPAddress my = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    if (mask[0] != 255 || mask[1] != 255 || mask[2] != 255) {
        Serial.println("[inv][scan] non-/24 mask, skipping active scan");
        return;
    }
    // IPs, die bereits aus knownIpProbe_/SSDP im Inventory sind, koennen
    // wir hier ueberspringen — spart ~3 s pro bereits gefundenem Speaker.
    bool already[256] = {};
    already[my[3]] = true;       // selbst
    {
        LockGuard g(*this);
        for (auto& s : speakers_) {
            IPAddress addr;
            if (addr.fromString(s.ip) && addr[0] == my[0] &&
                addr[1] == my[1] && addr[2] == my[2]) {
                already[addr[3]] = true;
            }
        }
    }
    Serial.printf("[inv][scan] active scan %d.%d.%d.1-254 (skipping %d known) ...\n",
                  my[0], my[1], my[2],
                  (int)std::count(already, already + 256, true) - 1);
    int found = 0;
    for (int i = 1; i <= 254; ++i) {
        if (already[i]) continue;
        char ip[20];
        snprintf(ip, sizeof(ip), "%d.%d.%d.%d", my[0], my[1], my[2], i);
        Speaker s;
        // Moderate Timeouts fuer noch laggy WiFi-Verbindungen (C6/C3 mit
        // WiFi 6 koennen in der Anfangsphase nach Boot 500 ms+ brauchen
        // bis SYN-ACK). 300 ms connect / 1200 ms read ergibt im worst
        // case 254 × 300 ≈ 76 s im Hintergrund-Task.
        if (probeIp_(ip, s, 300, 1200)) {
            ++found;
            Serial.printf("[inv][scan] %s -> %s (%s)\n",
                          ip, s.name.c_str(), s.model.c_str());
            mergeSpeaker_(s);
        }
        // kleiner yield damit AsyncTCP nicht ausgehungert wird
        if ((i & 0xF) == 0) delay(1);
    }
    Serial.printf("[inv][scan] active scan done, %d new speakers found\n", found);
}

void SpeakerInventory::activeScanTask_(void* arg) {
    auto* inv = static_cast<SpeakerInventory*>(arg);
    inv->activeScan_();
    inv->refreshMigrationStatus();  // gleich am Ende auch Status aktualisieren
    inv->saveToNVS();
    inv->scanRunning_.store(false);
    Serial.println("[inv] background scan task finished");
    vTaskDelete(nullptr);
}

MigrationStatus SpeakerInventory::detectStatus(const String& ip, const String& myBaseUrl,
                                                String* outCloudUrl) {
    if (outCloudUrl) *outCloudUrl = "";
    WiFiClient c;
    c.setTimeout(3);
    if (!c.connect(ip.c_str(), BOSE_TELNET_PORT, 3000)) {
        return MigrationStatus::OFFLINE;
    }
    delay(200);
    while (c.available()) c.read();  // banner verwerfen
    String reply;
    if (!telnetSend(c, "getpdo CurrentSystemConfiguration", reply, 5000)) {
        c.stop();
        return MigrationStatus::UNKNOWN;
    }
    c.print("trigger close\n");
    delay(50);
    c.stop();

    // Extrahiere margeServerUrl-Wert aus reply:
    //   margeServerUrl {
    //     text: "https://streaming.bose.com"
    //   }
    int mPos = reply.indexOf("margeServerUrl");
    if (mPos >= 0) {
        int tPos = reply.indexOf("text:", mPos);
        if (tPos >= 0) {
            int q1 = reply.indexOf('"', tPos);
            int q2 = (q1 >= 0) ? reply.indexOf('"', q1 + 1) : -1;
            if (q1 >= 0 && q2 > q1 && outCloudUrl) {
                *outCloudUrl = reply.substring(q1 + 1, q2);
            }
        }
    }
    // Originalzustand = zeigt auf Bose-Cloud-Hostnames
    if (reply.indexOf("streaming.bose.com") >= 0) return MigrationStatus::NOT_MIGRATED;
    // Alles andere mit http:// in margeServerUrl = irgendeine Replacement-Cloud
    // (ggf. WIR mit veralteter IP nach IP-Wechsel, ggf. ein anderes Geraet wie Pi5).
    // Die Unterscheidung "gehoert uns / gehoert wem-anders" macht das UI via
    // ownedByUs-Flag - hier nur der bloecke Telnet-Befund.
    if (outCloudUrl && outCloudUrl->length() > 0 &&
        outCloudUrl->startsWith("http")) return MigrationStatus::MIGRATED;
    return MigrationStatus::UNKNOWN;
}

void SpeakerInventory::refreshMigrationStatus() {
    // Snapshot unter Lock, dann Telnet-Calls OHNE Lock — sonst blockt jeder
    // Reader (AsyncTCP-Handler, Bose-Cloud-Mock) fuer 3-5 s pro Speaker.
    // Ergebnisse danach unter Lock wieder ins inventory mergen.
    std::vector<Speaker> work;
    { LockGuard g(*this); work = speakers_; }
    String myBase = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
    for (auto& s : work) {
        s.status = detectStatus(s.ip, myBase, &s.cloudUrl);
    }
    LockGuard g(*this);
    for (auto& upd : work) {
        // Re-find: deviceId kann sich nicht aendern, slot nach reload aber schon.
        Speaker* live = nullptr;
        for (auto& s : speakers_) {
            if (s.deviceId == upd.deviceId) { live = &s; break; }
        }
        if (!live) continue;
        live->status   = upd.status;
        live->cloudUrl = upd.cloudUrl;
        // Auto-Claim: zeigt eh schon auf UNS, aber ownedByUs noch False
        // (Edge-Case nach Flash-Erase/NVS-Reset) — uebernehmen wir.
        if (!live->ownedByUs && live->cloudUrl == myBase) {
            Serial.printf("[inv] auto-claim %s (cloudUrl matches our base)\n",
                          live->name.c_str());
            live->ownedByUs = true;
            continue;
        }
        // Auto-Release: zeigt auf eine ANDERE Cloud (anderes ESP oder
        // streaming.bose.com nach Revert) — wir sind nicht mehr zustaendig.
        // Verhindert dass ip_failsafe ihn beim naechsten IP-Wechsel
        // zurueck-claimt und dass das UI ihn faelschlich als unsere
        // Verantwortung zeigt. cloudUrl leer = Speaker offline -> kein Change.
        if (live->ownedByUs && live->cloudUrl.length() > 0 && live->cloudUrl != myBase) {
            Serial.printf("[inv] auto-release %s (cloudUrl=%s != our base %s)\n",
                          live->name.c_str(), live->cloudUrl.c_str(), myBase.c_str());
            live->ownedByUs = false;
        }
    }
    saveToNVS();
}

void SpeakerInventory::discoverSync() {
    // Light variant fuer Cron-Ticks: nur Sync-Phasen, kein Background-Scan.
    Serial.println("[inv] discoverSync (light)");
    knownIpProbe_();
    ssdpMSearch_();
    refreshMigrationStatus();
    Serial.printf("[inv] discoverSync done, %u speakers known\n",
                  (unsigned)speakers_.size());
}

void SpeakerInventory::discover() {
    // scanRunning_ ist *uebergreifend* fuer Sync-Phase + Active-Scan-Phase:
    // damit handleDiscover() im HTTP-Handler sofort returnen kann, das UI
    // scan_in_progress sieht und solange pollt bis beide Phasen durch sind.
    bool expected = false;
    if (!scanRunning_.compare_exchange_strong(expected, true)) {
        Serial.println("[inv] discover() already in flight, skipping");
        return;
    }
    Serial.println("[inv] discovery starting (sync phase)");
    // Synchrone Phase, ~5 s: bekannte IPs + SSDP-Burst. Findet zuverlaessig
    // alles was schon mal "gesehen" wurde plus alles was per SSDP-Multicast
    // pingt. Reicht in 99 % der Faelle.
    knownIpProbe_();
    ssdpMSearch_();
    refreshMigrationStatus();
    Serial.printf("[inv] sync phase done, %u speakers known\n",
                  (unsigned)speakers_.size());

    // Active-Scan in Hintergrund-Task — kann je nach LAN-Groesse 30 s+
    // dauern. UI pollt /api/speakers solange isScanRunning() == true.
    // scanRunning_ ist bereits gesetzt, activeScanTask_ released es am Ende.
    BaseType_t r = xTaskCreate(activeScanTask_, "boseScan",
                                6144, this, 1, nullptr);
    if (r != pdPASS) {
        Serial.println("[inv] xTaskCreate failed — running active scan synchronously");
        activeScan_();
        refreshMigrationStatus();
        saveToNVS();
        scanRunning_.store(false);
    }
}

bool SpeakerInventory::addByIp(const String& ip) {
    Speaker s;
    if (!probeIp_(ip, s)) return false;
    String myBase = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
    // detectStatus ist Telnet — bewusst OHNE Lock; danach unter Lock mergen.
    String cloudUrl;
    MigrationStatus st = detectStatus(ip, myBase, &cloudUrl);
    LockGuard g(*this);
    mergeSpeaker_(s);
    if (Speaker* p = findByIp(ip)) {
        p->status   = st;
        p->cloudUrl = cloudUrl;
    }
    saveToNVS();
    return true;
}

bool SpeakerInventory::remove(const String& deviceId) {
    LockGuard g(*this);
    for (auto it = speakers_.begin(); it != speakers_.end(); ++it) {
        if (it->deviceId == deviceId) {
            speakers_.erase(it);
            saveToNVS();
            return true;
        }
    }
    return false;
}

// HINWEIS: Caller muss SpeakerInventory::LockGuard halten waehrend der
// zurueckgegebene Pointer benutzt wird — sonst race mit mergeSpeaker_/erase.
Speaker* SpeakerInventory::findById(const String& deviceId) {
    for (auto& s : speakers_) {
        if (s.deviceId == deviceId) return &s;
    }
    return nullptr;
}

Speaker* SpeakerInventory::findByIp(const String& ip) {
    for (auto& s : speakers_) {
        if (s.ip == ip) return &s;
    }
    return nullptr;
}

std::vector<Speaker> SpeakerInventory::list() {
    LockGuard g(*this);
    return speakers_;
}

} // namespace bosefix

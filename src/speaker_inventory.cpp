// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "speaker_inventory.h"
#include "nvs_helper.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <esp_log.h>

namespace sixback {

namespace {

constexpr const char* NVS_NS  = "sixback-inv";
constexpr const char* NVS_KEY = "speakers";     // Legacy-Gesamtblob (<= v0.8.42)
constexpr const char* NVS_ORDER_KEY = "order";  // Anzeige-Reihenfolge der Slices
// Meta-Keys im Namespace, die bei der Slice-Enumeration nie als deviceId
// gelten duerfen (nvsListAbBaseIds-Exclude).
constexpr const char* kInvMetaKeys[] = { NVS_KEY, NVS_ORDER_KEY };

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

// SSDP-Sammelphase, bewusst als EIGENE Funktion (nicht inline in
// ssdpMSearch_): M-SEARCH-Burst senden, 4 s lauschen und die eindeutigen
// Responder-IPs einsammeln. Sobald diese Funktion zurueckkehrt, sind der
// 1-KB-Empfangspuffer + der WiFiUDP-Frame garantiert vom Stack verschwunden.
// Frueher wurde probeIp_() (HTTPClient -> lwIP, ~1.5-2 KB) noch INNERHALB der
// Empfangsschleife aufgerufen, also mit buf[1024] live darunter — das sprengte
// auf grossen Setups (9 Speaker) den 4-KB-Stack des bg-discover-Tasks:
// "Stack canary watchpoint triggered (bg-discover)" in v0.8.4.
void ssdpCollectCandidates_(std::vector<String>& outIps) {
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
        // Nur SoundTouch-typische Responses mit Location-Header beachten.
        // Bose-Speaker liefern z.B. http://192.168.1.50:8091/XD/BO5EBO5E-...
        // strstr auf dem NUL-terminierten buf statt String(buf) -> spart die
        // 1-KB-Heap-Kopie pro Paket.
        if (!strstr(buf, "Location: http://")) continue;
        String srcIp = src.toString();
        // De-Dup: 3x-Burst + Mehrfach-Antworten pro Geraet wuerden sonst
        // denselben Speaker mehrfach proben.
        bool dup = false;
        for (const auto& ip : outIps) { if (ip == srcIp) { dup = true; break; } }
        if (!dup) outIps.push_back(srcIp);
    }
    udp.stop();
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
        case MigrationStatus::SETTLING:       return "settling";
        default:                              return "unknown";
    }
}

const char* probeFailReasonStr(ProbeFailReason r) {
    switch (r) {
        case ProbeFailReason::OK:             return "ok";
        case ProbeFailReason::HTTP_BEGIN:     return "http_begin_failed";
        case ProbeFailReason::CONNECT_FAILED: return "connect_or_read_failed";
        case ProbeFailReason::HTTP_NOT_200:   return "http_not_200";
        case ProbeFailReason::EMPTY_BODY:     return "empty_body";
        case ProbeFailReason::WRONG_BODY:     return "wrong_body";
        case ProbeFailReason::NO_DEVICE_ID:   return "no_device_id";
        default:                              return "unknown";
    }
}

// deviceID-Chokepoint-Validierung: die ID landet UI-seitig in onclick-Strings,
// Attribut-Selektoren und URL-Pfaden — ein feindliches Geraet mit praeparierter
// /info-deviceID (Quotes/Klammern) waere stored XSS. Echte SoundTouch-IDs sind
// 12 Hex-Zeichen; wir lassen defensiv alnum bis 24 zu und blocken alles andere
// an der EINZIGEN Einlese-Stelle (probeIp_) + beim NVS-Load (Altbestand).
static bool deviceIdSafe_(const String& id) {
    if (id.length() == 0 || id.length() > 24) return false;
    for (size_t i = 0; i < id.length(); i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) return false;
    }
    return true;
}

// Ein Speaker als JSON-Objekt — dieselbe Form als Array-Element im
// Legacy-Gesamtblob "speakers" (<= v0.8.42) und als Root-Objekt eines
// Per-Speaker-Keys (Key = deviceId, seit dem Slicing). deviceId steht
// redundant im Slice (self-describing, wie beim PresetStore).
// Runtime-only-Felder (sourcesReady, offlineStreak, lastSeenMs sowie
// moduleType/variant aus /info) werden bewusst nicht persistiert.
static void buildSpeakerJson_(const Speaker& s, JsonObject o) {
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
    if (s.hidden) o["hidden"] = true;   // nur wenn gesetzt — spart Blob-Bytes
    if (!s.mediaServerUuids.empty()) {
        JsonArray msa = o["mediaServerUuids"].to<JsonArray>();
        for (const auto& uuid : s.mediaServerUuids) msa.add(uuid);
    }
    if (!s.spotifyAccounts.empty()) {
        JsonArray spa = o["spotifyAccounts"].to<JsonArray>();
        for (const auto& sa : s.spotifyAccounts) {
            JsonObject sp = spa.add<JsonObject>();
            sp["sourceAccount"] = sa.sourceAccount;
            sp["displayName"]   = sa.displayName;
        }
    }
}

// Gegenstueck zu buildSpeakerJson_. false = Eintrag unbrauchbar (unsafe
// deviceId, Chokepoint s.o.) — Caller ueberspringt ihn.
static bool parseSpeakerJson_(JsonObject o, Speaker& s) {
    s.deviceId   = (const char*)o["deviceId"];
    if (!deviceIdSafe_(s.deviceId)) {   // Altbestand-Hygiene (Chokepoint s.o.)
        Serial.printf("[inv] NVS entry with unsafe deviceID skipped\n");
        return false;
    }
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
    s.hidden     = o["hidden"] | false;
    if (o["mediaServerUuids"].is<JsonArray>()) {
        for (JsonVariant v : o["mediaServerUuids"].as<JsonArray>()) {
            s.mediaServerUuids.emplace_back((const char*)v);
        }
    }
    if (o["spotifyAccounts"].is<JsonArray>()) {
        for (JsonObject sp : o["spotifyAccounts"].as<JsonArray>()) {
            Speaker::SpotifyAccount sa;
            sa.sourceAccount = (const char*)(sp["sourceAccount"] | "");
            sa.displayName   = (const char*)(sp["displayName"]   | "");
            if (sa.sourceAccount.length() > 0) s.spotifyAccounts.push_back(sa);
        }
    }
    return true;
}

void SpeakerInventory::loadFromNVS() {
    LockGuard g(*this);
    speakers_.clear();
    // 1) Per-Speaker-Slices (Key = deviceId).
    std::vector<String> ids;
    nvsListAbBaseIds(NVS_NS, kInvMetaKeys, 2, ids);
    size_t sliced = 0;
    for (auto& id : ids) {
        JsonDocument doc;
        if (!nvsLoadJson(NVS_NS, id.c_str(), doc)) {
            // Slice unlesbar: skip — Discovery + mergeSpeaker_ regenerieren
            // den Eintrag, der naechste saveToNVS ersetzt den Key.
            Serial.printf("[inv] slice %s unreadable — skipped\n", id.c_str());
            continue;
        }
        Speaker s;
        if (!parseSpeakerJson_(doc.as<JsonObject>(), s)) continue;
        speakers_.push_back(s);
        ++sliced;
    }
    // 2) Legacy-Gesamtblob (<= v0.8.42): beim ersten Boot nach dem Update
    //    die einzige Quelle; nach einer UNTERBROCHENEN Migration (Reboot
    //    mitten in Phase A) liegen Blob UND Teil-Slices gleichzeitig —
    //    dann ergaenzt der Blob die noch fehlenden Speaker, nichts geht
    //    verloren. (Nur die Reihenfolge der Nachzuegler ist dann
    //    best-effort, bis der User neu sortiert.)
    JsonDocument legacy;
    const bool haveLegacy = nvsLoadJson(NVS_NS, NVS_KEY, legacy);
    if (haveLegacy) {
        for (JsonObject o : legacy["speakers"].as<JsonArray>()) {
            Speaker s;
            if (!parseSpeakerJson_(o, s)) continue;
            bool known = false;
            for (auto& e : speakers_) {
                if (e.deviceId == s.deviceId) { known = true; break; }
            }
            if (!known) speakers_.push_back(s);
        }
    }
    if (speakers_.empty() && !haveLegacy) {
        Serial.println("[inv] no NVS-state, starting fresh");
        return;
    }
    // 3) Anzeige-Reihenfolge wiederherstellen — die Slice-Enumeration ist
    //    NVS-Iterationsreihenfolge, nicht die des Users.
    JsonDocument od;
    if (nvsLoadJson(NVS_NS, NVS_ORDER_KEY, od)) {
        std::vector<String> order;
        for (JsonVariant v : od["ids"].as<JsonArray>()) {
            order.emplace_back((const char*)v);
        }
        applyOrder_(order);
    }
    Serial.printf("[inv] loaded %u speakers from NVS (%u sliced%s)\n",
                  (unsigned)speakers_.size(), (unsigned)sliced,
                  haveLegacy ? ", legacy blob present" : "");
    if (haveLegacy) {
        if (migrationFits_(ids)) {
            migrateLegacyBlob_();
        } else {
            // Zu voll fuer eine sichere Migration: im Blob-Modus bleiben —
            // Verhalten wie vor dem Slicing, null Regression. Teil-Slices
            // einer frueher unterbrochenen Migration zuruecknehmen (Erase
            // schafft Platz, und der Zustand ist wieder deterministisch);
            // ihr Inhalt steht laengst im RAM-Union von oben.
            slicedMode_ = false;
            for (auto& id : ids) nvsEraseAbKeys(NVS_NS, id.c_str());
            nvsEraseAbKeys(NVS_NS, NVS_ORDER_KEY);
            Serial.println("[inv] migration guard: STAY-LEGACY — Gesamtblob-"
                           "Persistenz bleibt aktiv, naechster Boot prueft neu");
        }
    }
}

bool SpeakerInventory::migrationFits_(const std::vector<String>& haveSliceIds) {
    size_t need = 0;
    JsonDocument doc;
    for (auto& s : speakers_) {
        bool have = false;
        for (auto& id : haveSliceIds) {
            if (id == s.deviceId) { have = true; break; }
        }
        if (have) continue;   // Unchanged-Skip macht vorhandene Slices gratis
        doc.clear();
        buildSpeakerJson_(s, doc.to<JsonObject>());
        need += nvsEntriesNeededForBlob(measureJson(doc));
    }
    {   // Order-Key
        doc.clear();
        JsonArray a = doc["ids"].to<JsonArray>();
        for (auto& s : speakers_) a.add(s.deviceId);
        need += nvsEntriesNeededForBlob(measureJson(doc));
    }
    const size_t avail   = nvsAvailableEntries();
    if (avail == SIZE_MAX) return true;   // Stats kaputt: nicht blocken (wie Gate)
    const size_t reclaim = nvsAbBlobEntries(NVS_NS, NVS_KEY);
    const bool fits = need <= avail + reclaim;
    Serial.printf("[inv] migration guard: brauche ~%u entries, verfuegbar %u "
                  "+ %u blob-reclaim -> %s\n",
                  (unsigned)need, (unsigned)avail, (unsigned)reclaim,
                  fits ? "GO" : "STAY-LEGACY");
    return fits;
}

void SpeakerInventory::migrateLegacyBlob_() {
    // Phase A: Slices NEBEN den Legacy-Blob schreiben. Der Unchanged-Skip
    // im A/B-Save macht bei einer wiederaufgenommenen Migration die schon
    // vorhandenen Slices gratis.
    std::vector<String> failed;
    for (auto& s : speakers_) {
        if (!saveSpeakerToNVS_(s)) failed.push_back(s.deviceId);
    }
    bool orderOk = saveOrderToNVS_();
    if (failed.empty() && orderOk) {
        nvsEraseAbKeys(NVS_NS, NVS_KEY);
        Serial.printf("[inv] migrated %u speaker(s) to per-speaker keys, "
                      "legacy blob removed\n", (unsigned)speakers_.size());
        return;
    }
    // Druck-Migration (PresetStore-Muster): Gesamtblob + alle Slices passen
    // nicht GLEICHZEITIG in die Partition — Blob zuerst loeschen (der Stand
    // liegt vollstaendig im RAM), dann die Nachzuegler erneut.
    Serial.printf("[inv] migrate phase A: %u slice(s)%s passten nicht NEBEN "
                  "den legacy blob — Druck-Migration\n",
                  (unsigned)failed.size(), orderOk ? "" : " + order-key");
    nvsEraseAbKeys(NVS_NS, NVS_KEY);
    size_t still = 0;
    for (auto& id : failed) {
        Speaker* sp = findById(id);   // Lock haelt der loadFromNVS-Caller
        if (!sp || !saveSpeakerToNVS_(*sp)) {
            ++still;
            Serial.printf("[inv] migrate FAIL %s — Slice bleibt RAM-only "
                          "bis Platz frei wird\n", id.c_str());
        }
    }
    if (!orderOk) orderOk = saveOrderToNVS_();
    if (still == 0 && orderOk) {
        Serial.printf("[inv] migrated %u speaker(s) via Druck-Migration, "
                      "legacy blob removed\n", (unsigned)speakers_.size());
    } else {
        Serial.printf("[inv] migration UNVOLLSTAENDIG: %u Slice(s) RAM-only "
                      "(NVS weiterhin zu voll)\n", (unsigned)still);
    }
}

bool SpeakerInventory::saveSpeakerToNVS_(const Speaker& s) {
    if (s.deviceId.length() == 0 || s.deviceId.length() > 15) {
        // NVS-Keys enden bei 15 Zeichen; echte deviceIds (12-hex-MAC)
        // passen immer. deviceIdSafe_ liesse bis 24 zu — so ein Exot
        // landet hier und ist ein loggenswerter Sonderfall, kein Crash.
        Serial.printf("[inv] saveSpeaker REJECT: deviceId '%s' ist kein "
                      "gueltiger NVS-Key\n", s.deviceId.c_str());
        return false;
    }
    JsonDocument doc;
    buildSpeakerJson_(s, doc.to<JsonObject>());
    // Analog PresetStore (FHEM 144729 #153): ein bei Heap-Knappheit
    // ueberlaufenes JsonDocument (ArduinoJson dropt Nodes still) wuerde
    // als VALIDES Teil-JSON committed — lieber gar nicht schreiben, der
    // naechste Save heilt.
    if (doc.overflowed()) {
        Serial.println("[inv] saveSpeaker ABORT: JsonDocument overflowed "
                       "(heap zu knapp) — NVS-Stand bleibt unangetastet");
        return false;
    }
    return nvsSaveJsonWithCleanup(NVS_NS, s.deviceId.c_str(), doc);
}

bool SpeakerInventory::saveOrderToNVS_() {
    if (speakers_.empty()) {
        // Leeres Inventory: Order-Key entsorgen statt {"ids":[]} pflegen.
        nvsEraseAbKeys(NVS_NS, NVS_ORDER_KEY);
        return true;
    }
    JsonDocument doc;
    JsonArray a = doc["ids"].to<JsonArray>();
    for (auto& s : speakers_) a.add(s.deviceId);
    return nvsSaveJsonWithCleanup(NVS_NS, NVS_ORDER_KEY, doc);
}

bool SpeakerInventory::saveLegacyBlobToNVS_() {
    // Fallback fuer Partitionen, auf denen migrationFits_ die Slice-
    // Migration abgelehnt hat: Gesamtblob-Save wie vor dem Slicing.
    JsonDocument doc;
    JsonArray arr = doc["speakers"].to<JsonArray>();
    for (auto& s : speakers_) {
        buildSpeakerJson_(s, arr.add<JsonObject>());
    }
    bool ok = !doc.overflowed() && nvsSaveJsonWithCleanup(NVS_NS, NVS_KEY, doc);
    if (!ok) {
        saveFails_++;
        Serial.printf("[inv] saveToNVS FAILED (#%u, legacy mode)\n",
                      (unsigned)saveFails_);
    }
    return ok;
}

bool SpeakerInventory::saveToNVS() {
    LockGuard g(*this);
    if (!slicedMode_) return saveLegacyBlobToNVS_();
    // Per-Slice-Sweep statt Gesamtblob: der A/B-Peak ist damit die groesste
    // Einzel-Slice, nicht mehr alt+neu des groessten Einzel-Writes im
    // System (der 9-Speaker-Blob; freds 6 inventory.save_fails auf v0.8.41).
    // Der Unchanged-Skip im A/B-Save laesst ungeaenderte Slices aus — real
    // geschrieben wird nur, was sich geaendert hat.
    bool all = true;
    for (auto& s : speakers_) {
        all = saveSpeakerToNVS_(s) && all;
    }
    all = saveOrderToNVS_() && all;
    // Verwaiste Slices wegputzen (remove()'te Speaker) — sonst stuenden
    // sie beim naechsten Boot wieder auf.
    std::vector<String> ids;
    nvsListAbBaseIds(NVS_NS, kInvMetaKeys, 2, ids);
    for (auto& id : ids) {
        if (!findById(id)) nvsEraseAbKeys(NVS_NS, id.c_str());
    }
    // Nach einem vollstaendig persistierten Stand ist ein evtl. noch
    // liegender Legacy-Blob obsolet (isKey-Guard -> no-op im Normalfall).
    if (all) nvsEraseAbKeys(NVS_NS, NVS_KEY);
    if (!all) {
        // Sichtbar machen statt still verlieren (Analog preset_store.save_fails;
        // die Callsites melden dem Client sonst 200 trotz verlorenem Write).
        // Anders als beim Gesamtblob sind die uebrigen Slices trotzdem
        // persistiert — ein Fail kostet einen Speaker, nicht alle.
        saveFails_++;
        Serial.printf("[inv] saveToNVS FAILED (#%u)\n", (unsigned)saveFails_);
    }
    return all;
}

void SpeakerInventory::mergeSpeaker_(const Speaker& s) {
    LockGuard g(*this);
    for (auto& existing : speakers_) {
        if (existing.deviceId == s.deviceId) {
            existing.name       = s.name;
            existing.model      = s.model;
            existing.firmware   = s.firmware;
            existing.moduleType = s.moduleType;
            existing.variant    = s.variant;
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
                                 uint16_t connectMs, uint16_t readMs,
                                 ProbeFailure* failOut) {
    auto setFail = [&](ProbeFailReason r, const String& d) {
        if (failOut) { failOut->reason = r; failOut->detail = d; }
    };
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(connectMs);
    http.setTimeout(readMs);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/info";
    if (!http.begin(url)) {
        Serial.printf("[probe] %s http.begin() failed (URL invalid?)\n", ip.c_str());
        setFail(ProbeFailReason::HTTP_BEGIN, "http.begin() returned false");
        return false;
    }
    uint32_t t0 = millis();
    int code = http.GET();
    uint32_t dt = millis() - t0;
    if (code <= 0) {
        // Negative codes = HTTPClient internal errors (-1 connect, -11 read-timeout, etc.)
        Serial.printf("[probe] %s connect/read failed (%d) after %u ms — timeout=%u/%u\n",
                      ip.c_str(), code, dt, connectMs, readMs);
        http.end();
        setFail(ProbeFailReason::CONNECT_FAILED,
                String("HTTP client error code=") + String(code) +
                ", elapsed=" + String(dt) + "ms, connectTimeout=" +
                String(connectMs) + "ms readTimeout=" + String(readMs) + "ms");
        return false;
    }
    if (code != 200) {
        Serial.printf("[probe] %s HTTP %d (expected 200) after %u ms\n",
                      ip.c_str(), code, dt);
        http.end();
        setFail(ProbeFailReason::HTTP_NOT_200,
                String("HTTP ") + String(code) + " (expected 200)");
        return false;
    }
    String xml = http.getString();
    http.end();
    if (xml.length() == 0) {
        Serial.printf("[probe] %s empty body after HTTP 200\n", ip.c_str());
        setFail(ProbeFailReason::EMPTY_BODY, "HTTP 200 but empty body");
        return false;
    }
    if (xml.indexOf("<info") < 0) {
        Serial.printf("[probe] %s body has no <info ...> tag (len=%u, first=%.60s)\n",
                      ip.c_str(), (unsigned)xml.length(),
                      xml.c_str() ? xml.c_str() : "");
        setFail(ProbeFailReason::WRONG_BODY,
                String("HTTP 200 but body lacks <info ...> tag (len=") +
                String(xml.length()) + ")");
        return false;
    }
    out.deviceId  = xmlAttr(xml, "deviceID");
    out.name      = xmlValue(xml, "name");
    out.model     = xmlValue(xml, "type");
    out.firmware  = xmlValue(xml, "softwareVersion");
    out.moduleType = xmlValue(xml, "moduleType");   // sm2/scm — HW-Revision (Issue-Triage)
    out.variant    = xmlValue(xml, "variant");      // rhino/mojo/spotty
    out.accountId = xmlValue(xml, "margeAccountUUID");
    out.ip        = ip;
    out.status    = MigrationStatus::UNKNOWN;
    if (out.deviceId.length() == 0) {
        Serial.printf("[probe] %s <info> present but deviceID attribute empty\n", ip.c_str());
        setFail(ProbeFailReason::NO_DEVICE_ID,
                "<info> tag present but deviceID attribute empty");
        return false;
    }
    if (!deviceIdSafe_(out.deviceId)) {
        Serial.printf("[probe] %s deviceID rejected (non-alnum or >24 chars)\n", ip.c_str());
        setFail(ProbeFailReason::NO_DEVICE_ID,
                "deviceID contains invalid characters (alnum, max 24 expected)");
        return false;
    }
    Serial.printf("[probe] %s OK after %u ms — %s (%s)\n",
                  ip.c_str(), dt, out.name.c_str(), out.model.c_str());
    if (failOut) failOut->reason = ProbeFailReason::OK;
    return true;
}

void SpeakerInventory::ssdpMSearch_() {
    // Phase 1 (eigener Stack-Frame): Burst senden + Responder-IPs sammeln.
    // buf[1024] + WiFiUDP leben NUR in ssdpCollectCandidates_ und sind nach
    // dessen Return schon wieder freigegeben.
    std::vector<String> candidates;
    ssdpCollectCandidates_(candidates);
    // Phase 2: jeden Kandidaten proben — jetzt OHNE den 1-KB-Empfangspuffer
    // auf dem Stack, sodass der HTTPClient-Frame in probeIp_ Luft hat.
    for (const String& srcIp : candidates) {
        Speaker s;
        if (probeIp_(srcIp, s)) {
            Serial.printf("[inv][ssdp] %s -> %s (%s)\n",
                          srcIp.c_str(), s.name.c_str(), s.model.c_str());
            mergeSpeaker_(s);
        }
    }
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
    int toScan = 254 - (int)std::count(already, already + 256, true);
    Serial.printf("[inv][scan] active scan %d.%d.%d.1-254 (skipping %d known, %d to probe) ...\n",
                  my[0], my[1], my[2],
                  (int)std::count(already, already + 256, true) - 1,
                  toScan);

    // Network/HTTPClient-Log-Spam waehrend des /24-Scans unterdruecken:
    // jeder Nicht-Bose-Host loggt sonst [E] socket-reset + [W] connection-
    // refused + [I] timeout — ca. 1000 Zeilen pro Scan. Wir wollen nur
    // unsere eigene Progress-Anzeige sehen. NACH dem Scan sofort zurueck-
    // setzen, damit echte HTTP-Fehler (z.B. push-to-device) wieder sichtbar
    // werden.
    esp_log_level_t prev_net  = esp_log_level_get("NetworkClient");
    esp_log_level_t prev_http = esp_log_level_get("HTTPClient");
    esp_log_level_set("NetworkClient", ESP_LOG_NONE);
    esp_log_level_set("HTTPClient",    ESP_LOG_NONE);

    int found     = 0;
    int probed    = 0;
    uint32_t lastDot = millis();
    Serial.print("[inv][scan] progress: ");
    for (int i = 1; i <= 254; ++i) {
        if (already[i]) continue;
        char ip[20];
        snprintf(ip, sizeof(ip), "%d.%d.%d.%d", my[0], my[1], my[2], i);
        Speaker s;
        // Moderate Timeouts fuer noch laggy WiFi-Verbindungen (C6/C3 mit
        // WiFi 6 koennen in der Anfangsphase nach Boot 500 ms+ brauchen
        // bis SYN-ACK). 300 ms connect / 1200 ms read ergibt im worst
        // case 254 × 300 ≈ 76 s im Hintergrund-Task.
        bool hit = probeIp_(ip, s, 300, 1200);
        ++probed;
        if (hit) {
            ++found;
            // Newline vor Speaker-Treffer damit Progress-Punkte sauber bleiben:
            Serial.printf("\n[inv][scan] %d/%d HIT %s -> %s (%s)\n",
                          probed, toScan, ip, s.name.c_str(), s.model.c_str());
            Serial.print("[inv][scan] progress: ");
            mergeSpeaker_(s);
        } else if (millis() - lastDot > 2000) {
            // alle 2 s einen Progress-Punkt setzen, damit man sieht
            // dass der Scan lebt (nicht haengt). Counter alle 10 Probes:
            if (probed % 10 == 0) {
                Serial.printf("%d", probed);
            } else {
                Serial.print(".");
            }
            lastDot = millis();
        }
        // kleiner yield damit AsyncTCP nicht ausgehungert wird
        if ((i & 0xF) == 0) delay(1);
    }
    Serial.printf("\n[inv][scan] active scan done, %d/%d probed, %d new speakers found\n",
                  probed, toScan, found);

    // Log-Level zurueck — damit "echte" HTTP-Errors aus push-to-device,
    // import-from-device, migrate-telnet etc. wieder im Serial-Log auftauchen.
    esp_log_level_set("NetworkClient", prev_net);
    esp_log_level_set("HTTPClient",    prev_http);
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

void SpeakerInventory::refreshStatusTask_(void* arg) {
    // Hintergrund-Variante des manuellen "Refresh status" (FHEM 144729 #86).
    // refreshMigrationStatus() persistiert selbst (saveToNVS am Ende), ebenso
    // refreshSpotifyAccounts() — daher hier kein zusaetzliches saveToNVS noetig.
    auto* inv = static_cast<SpeakerInventory*>(arg);
    inv->refreshMigrationStatus();
    // Stufe-0 Diagnose: pro Speaker die Spotify-Account-Verknuepfung neu pullen
    // (leichter BMX /sources-GET, ~3 s pro Speaker max). list() liefert eine
    // Kopie -> sicher iterierbar waehrend refreshSpotifyAccounts die Live-Liste
    // anfasst.
    for (const auto& s : inv->list()) {
        inv->refreshSpotifyAccounts(s.deviceId);
    }
    UBaseType_t freeBytes = uxTaskGetStackHighWaterMark(nullptr);
    Serial.printf("[inv] background refresh-status done — stack high-water-mark=%u bytes free (of 8192)\n",
                  (unsigned)freeBytes);
    inv->scanRunning_.store(false);
    vTaskDelete(nullptr);
}

MigrationStatus SpeakerInventory::detectStatus(const String& ip, const String& myBaseUrl,
                                                String* outCloudUrl) {
    if (outCloudUrl) *outCloudUrl = "";
    WiFiClient c;
    c.setTimeout(3);
    if (!c.connect(ip.c_str(), BOSE_TELNET_PORT, 3000)) {
        // Telnet (Diag-Shell auf Port 17000) ist transient unreachable z.B.
        // waehrend Cloud-Migration-Reboot oder hoher Speaker-Belastung. Vor
        // OFFLINE-Verdikt pruefen ob die BMX-API (Port 8090) noch antwortet —
        // wenn ja, lebt der Speaker, nur die Diag-Shell ist gerade nicht da.
        // → SETTLING statt OFFLINE, damit das UI keinen falschen "Speaker ist
        // aus"-Alarm wirft. Slot/Cloud-URL bleibt unbekannt bis Telnet wieder
        // antwortet.
        HTTPClient http;
        // 2026-06-10 (FHEM #49 fred): Port war faelschlich BOSE_HTTP_PORT (8000 =
        // unser EIGENER ESP-Port), nicht BOSE_BMX_PORT (8090 = Speaker-API). Der
        // Speaker bedient 8000 nicht -> dieser Fallback lieferte NIE 200 -> das
        // SETTLING-Verdikt (Speaker lebt, nur Telnet-Diag-Shell transient weg) war
        // faktisch tot, jeder Telnet-Blip kippte direkt auf OFFLINE. Alle anderen
        // /info-Proben nutzen korrekt 8090 (probeIp_, ip_failsafe, system_health,
        // auto_mode).
        String bmxUrl = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/info";
        if (http.begin(bmxUrl)) {
            http.setConnectTimeout(2000);  // war ungesetzt -> arduino-Default ~5s
            http.setTimeout(4000);         // war 2000 — spielende Box unter Last
                                           // braucht mehr Luft fuer die Antwort
            int code = http.GET();
            http.end();
            if (code == 200) return MigrationStatus::SETTLING;
        }
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

// Prueft ob ein zu UNS migrierter Speaker den SixBack-TUNEIN-Source als READY
// in /sources hat. Fehlt der (migriert, aber account/full nie angewendet —
// Issue #10: Speaker zeigt "migrated", aber /select TUNEIN -> 500 weil keine
// TUNEIN-Source registriert ist), liefert false -> UI warnt + bietet Re-Sync.
// Konservativ: unerreichbar / nicht-200 -> true (nicht faelschlich warnen).
static bool probeTuneinReady_(const String& ip) {
    HTTPClient http;
    http.setReuse(false);
    if (!http.begin("http://" + ip + ":" + String(BOSE_BMX_PORT) + "/sources")) return true;
    int code = http.GET();
    if (code != 200) { http.end(); return true; }
    String body = http.getString();
    http.end();
    int pos = body.indexOf("source=\"TUNEIN\"");
    while (pos >= 0) {
        int tagEnd = body.indexOf('>', pos);
        if (tagEnd > pos &&
            body.substring(pos, tagEnd).indexOf("status=\"READY\"") >= 0) return true;
        pos = body.indexOf("source=\"TUNEIN\"", pos + 15);
    }
    return false;  // kein READY-TUNEIN -> Re-Sync noetig
}

// Analog probeTuneinReady_, aber fuer STORED_MUSIC (DLNA-Presets, Issue #30).
// true = mindestens ein <sourceItem source="STORED_MUSIC" ... status="READY">.
// Anders als TUNEIN ist diese Source am Speaker NICHT reboot-persistent — sie
// haengt an unserem account/full-STORED_MUSIC-Block (gebaut aus
// mediaServerUuids) und geht nach einem Cold-Boot verloren, waehrend der
// Speaker die DLNA-Server weiter SIEHT (Browse ok) und TUNEIN READY bleibt.
// Der abschliessende Quote in "STORED_MUSIC\"" grenzt sauber gegen
// STORED_MUSIC_MEDIA_RENDERER ab. Konservativ: unerreichbar/non-200 -> true
// (nicht faelschlich heilen wollen).
static bool probeStoredMusicReady_(const String& ip) {
    HTTPClient http;
    http.setReuse(false);
    if (!http.begin("http://" + ip + ":" + String(BOSE_BMX_PORT) + "/sources")) return true;
    int code = http.GET();
    if (code != 200) { http.end(); return true; }
    String body = http.getString();
    http.end();
    int pos = body.indexOf("source=\"STORED_MUSIC\"");
    while (pos >= 0) {
        int tagEnd = body.indexOf('>', pos);
        if (tagEnd > pos &&
            body.substring(pos, tagEnd).indexOf("status=\"READY\"") >= 0) return true;
        pos = body.indexOf("source=\"STORED_MUSIC\"", pos + 21);
    }
    return false;  // kein READY-STORED_MUSIC -> DLNA-Source-Re-Sync noetig
}

// Synthetischer, deterministischer accountId aus der SCM-MAC (deviceId), fuer
// Speaker die (mehr) keinen margeAccountUUID haben. Numerisch (account-aehnlich),
// pro-Speaker eindeutig, kollisionsfrei. Ohne accountId ist ein Speaker nicht
// marge-"eligible" -> kein /setMargeAccount -> keine Source-Registrierung
// (Issue #10: JeyP91 ST20 hatte margeAccountUUID="" -> "no eligible speakers").
static String syntheticAccountId_(const String& deviceId) {
    uint64_t v = strtoull(deviceId.c_str(), nullptr, 16);
    if (v == 0) return String("sb") + deviceId;  // Fallback falls nicht-hex
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return String(buf);
}

// Bindet den Speaker per /setMargeAccount an den (SixBack-)accountId — identisch
// zur marge-keepalive. Danach pullt der Speaker /streaming/account/<id>/full und
// registriert die SixBack-Sources (TUNEIN/RB/LIR/STORED_MUSIC). true = HTTP 200.
static bool margeSetAccount_(const String& ip, const String& accountId) {
    if (ip.length() == 0 || accountId.length() == 0) return false;
    HTTPClient http;
    http.setReuse(false);
    if (!http.begin("http://" + ip + ":" + String(BOSE_BMX_PORT) + "/setMargeAccount")) return false;
    http.addHeader("Content-Type", "application/xml");
    String body = "<PairDeviceWithAccount><accountId>";
    body += accountId;
    body += "</accountId><userAuthToken>Bearer sixback-keepalive</userAuthToken></PairDeviceWithAccount>";
    int code = http.POST(body);
    http.end();
    return code == 200;
}

// Peer detection — probt ob unter `url` ein LEBENDER SixBack antwortet.
// GET url + "/" mit kurzem Timeout; die SixBack-Cloud-Mock liefert auf /
// ein HTML mit dem Marker "SixBack". Ein anderer Custom-Cloud-Server
// (Pi5-Mock z.B.) oder eine tote URL liefern den Marker nicht.
// Genutzt von auto_mode (peer-aware-Skip + Re-Claim) und vom (b)-Disown
// in refreshMigrationStatus (s.u.). Frueher anon in auto_mode.cpp.
bool isPeerSixBackCloud(const String& url) {
    if (url.length() == 0 || !url.startsWith("http://")) return false;
    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(1500);
    if (!http.begin(url + "/")) return false;
    int code = http.GET();
    String body = (code == 200) ? http.getString() : String("");
    http.end();
    return (code == 200) && (body.indexOf("SixBack") >= 0);
}

// Anzeige-Entprellung des Status-Tiles (FHEM 144729 #49, fred): wie viele
// aufeinanderfolgende Schlecht-Probe-Zyklen noetig sind, bevor die Kachel von
// einem gesunden Status auf OFFLINE/SETTLING kippt. 2 = ein einzelner verfehlter
// Durchlauf (Telnet-Blip / transienter /info-Timeout) bleibt unsichtbar, ein
// echter Ausfall zeigt sich nach <=2 Zyklen.
static constexpr uint8_t STATUS_OFFLINE_DEBOUNCE = 2;

void SpeakerInventory::refreshMigrationStatus() {
    // Snapshot unter Lock, dann Telnet-Calls OHNE Lock — sonst blockt jeder
    // Reader (AsyncTCP-Handler, Bose-Cloud-Mock) fuer 3-5 s pro Speaker.
    // Ergebnisse danach unter Lock wieder ins inventory mergen.
    std::vector<Speaker> work;
    { LockGuard g(*this); work = speakers_; }
    String myBase = "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
    for (auto& s : work) {
        s.status = detectStatus(s.ip, myBase, &s.cloudUrl);
        // Source-Health (Issue #10): nur fuer zu-UNS-migrierte Speaker pruefen,
        // ob TUNEIN als READY-Source registriert ist. Sonst (nicht-migriert /
        // peer / offline) irrelevant -> als gesund markieren.
        // NUR wenn WIR der Migrationspartner sind (Dirk 2026-06-01): ownedByUs
        // (wir haben ihn migriert) UND er zeigt aktuell auf unsere Base-URL.
        // Verhindert dass wir je einen Peer-migrierten Speaker anfassen
        // (/setMargeAccount waere "klauen"). cloudUrl==myBase allein reicht
        // nicht — bei Auto-Claim-Edge-Cases erst ab naechstem Refresh owned.
        const bool oursMigrated =
            (s.ownedByUs && s.status == MigrationStatus::MIGRATED && s.cloudUrl == myBase);
        s.sourcesReady = oursMigrated ? probeTuneinReady_(s.ip) : true;
        // AUTO-HEAL (Issue #10): ours+migrated, aber TUNEIN-Source fehlt ->
        // accountId sicherstellen (synthetisch aus deviceId wenn der Speaker
        // keinen margeAccountUUID hat) + /setMargeAccount pushen. Der Speaker
        // bindet dann an den Account, pullt account/full und registriert die
        // SixBack-Sources. Laeuft bei jedem Status-Refresh/Cron-Tick ->
        // SELBSTHEILEND ohne dass der User je die WebUI sehen muss. Idempotent;
        // sobald die Sources READY sind, greift die Bedingung nicht mehr.
        if (oursMigrated && !s.sourcesReady) {
            if (s.accountId.length() == 0) {
                s.accountId = syntheticAccountId_(s.deviceId);
                Serial.printf("[inv] auto-heal: assign synthetic accountId %s to %s "
                              "(empty margeAccountUUID -> was not marge-eligible)\n",
                              s.accountId.c_str(), s.name.c_str());
            }
            bool ok = margeSetAccount_(s.ip, s.accountId);
            Serial.printf("[inv] auto-heal: /setMargeAccount %s acct=%s -> %d "
                          "(sources re-register on next account/full pull)\n",
                          s.ip.c_str(), s.accountId.c_str(), (int)ok);
        }
        // AUTO-HEAL STORED_MUSIC (Issue #30): die DLNA-Source ist — anders als
        // TUNEIN (oben) — am Speaker NICHT reboot-persistent. Nach einem
        // Cold-Boot (Stromausfall) kann sie fehlen, obwohl der Speaker die
        // DLNA-Server weiter sieht (Browse + /listMediaServers ok) und TUNEIN
        // READY bleibt -> der DLNA-Preset-Push (recordDlnaWorker_) bricht mit
        // /select=500 1005 UNKNOWN_SOURCE ab ("speaker hardware out of sync").
        // Der TUNEIN-Heal oben deckt das nicht ab (prueft nur TUNEIN). Hier:
        // ist STORED_MUSIC nicht READY, refreshMediaServers (haelt unsere UUIDs
        // frisch) + /setMargeAccount (zwingt einen account/full-Re-Pull, bei dem
        // der Speaker STORED_MUSIC re-registriert). Nur wenn der Speaker
        // ueberhaupt DLNA-Server hat (sonst kein DLNA-User -> kein Heal). Lab-
        // verifiziert 2026-06-13: /select STORED_MUSIC 500 -> 200 nach diesem
        // Pfad. Idempotent + selbstheilend ueber Cron-Ticks; sobald READY,
        // greift es nicht mehr.
        if (oursMigrated && !probeStoredMusicReady_(s.ip)) {
            refreshMediaServers(s.deviceId);
            bool haveUuids = false;
            {
                LockGuard g(*this);
                if (Speaker* p = findById(s.deviceId))
                    haveUuids = !p->mediaServerUuids.empty();
            }
            if (haveUuids) {
                if (s.accountId.length() == 0)
                    s.accountId = syntheticAccountId_(s.deviceId);
                bool ok = margeSetAccount_(s.ip, s.accountId);
                Serial.printf("[inv] auto-heal STORED_MUSIC: %s /setMargeAccount=%d "
                              "(DLNA source re-registers on next account/full pull)\n",
                              s.ip.c_str(), (int)ok);
            }
        }
    }
    // Stale-view-fix (2026-05-26): die auto-release-Pruefung (s.u.) skipt
    // bei cloudUrl=="" — genau das ist aber der State waehrend ein Speaker
    // mid-Reboot von einem Migrate ist (OFFLINE/SETTLING). Ohne Retry-Pass
    // sieht die source-stick minutenlang "owned=true" obwohl der Speaker
    // schon auf einer anderen Cloud landed. Wir retryen daher bis 30s
    // (3 x 10s) genau die Speaker die WIR noch als owned tracken UND deren
    // letzter probe OFFLINE/SETTLING war — Symptom mid-Bose-Reboot.
    // Speaker mit MIGRATED/NOT_MIGRATED-Status haben cloudUrl gesetzt und
    // brauchen keinen Retry. Speaker die wir nie owned haben sind nicht
    // unsere Verantwortung — kein retry-cost.
    for (int retry = 1; retry <= 3; ++retry) {
        std::vector<size_t> retryIdx;
        for (size_t i = 0; i < work.size(); ++i) {
            if (work[i].cloudUrl.length() > 0) continue;
            if (work[i].status != MigrationStatus::SETTLING &&
                work[i].status != MigrationStatus::OFFLINE) continue;
            bool ownedNow = false;
            {
                LockGuard g(*this);
                Speaker* p = findById(work[i].deviceId);
                if (p) ownedNow = p->ownedByUs;
            }
            if (!ownedNow) continue;
            retryIdx.push_back(i);
        }
        if (retryIdx.empty()) break;
        Serial.printf("[inv] stale-view-fix: %u owned-stale speaker(s), retry %d/3 in 10s\n",
                      (unsigned)retryIdx.size(), retry);
        delay(10000);
        for (size_t i : retryIdx) {
            work[i].status = detectStatus(work[i].ip, myBase, &work[i].cloudUrl);
        }
    }
    // (b)-Disown-Semantik (2026-06-06, Discussion #19): ein Auto-Release
    // braucht einen VERIFIZIERTEN fremden Owner — lebender SixBack-Peer
    // (Marker-Probe) oder bewusster Bose-Revert. Eine TOTE fremde cloudUrl
    // ist ambig: unsere eigene alte IP nach DHCP-Wechsel ODER ein
    // entsorgter Zweit-Stick. In beiden Faellen behalten wir owned, damit
    // ip_failsafe (IP-Wechsel) bzw. auto_mode-Re-Claim den Speaker wieder
    // auf uns ziehen koennen, statt ihn dauerhaft zu verwaisen.
    // HTTP-Probe hier VOR dem Lock (1,5 s Timeout pro toter URL — darf
    // nicht unter dem Inventory-Lock laufen, gleiches Argument wie oben).
    std::vector<bool> foreignOwnerVerified(work.size(), false);
    for (size_t i = 0; i < work.size(); ++i) {
        const auto& s = work[i];
        if (!s.ownedByUs) continue;
        if (s.cloudUrl.length() == 0 || s.cloudUrl == myBase) continue;
        if (s.cloudUrl.indexOf("bose.com") >= 0) {       // Revert = User-Wille
            foreignOwnerVerified[i] = true;
            continue;
        }
        foreignOwnerVerified[i] = isPeerSixBackCloud(s.cloudUrl);
    }
    LockGuard g(*this);
    for (size_t i = 0; i < work.size(); ++i) {
        auto& upd = work[i];
        // Re-find: deviceId kann sich nicht aendern, slot nach reload aber schon.
        Speaker* live = nullptr;
        for (auto& s : speakers_) {
            if (s.deviceId == upd.deviceId) { live = &s; break; }
        }
        if (!live) continue;
        // Anzeige-Entprellung (FHEM #49 fred): ein einzelner verfehlter Probe-
        // Durchlauf darf das Tile NICHT sofort auf OFFLINE/SETTLING kippen, solange
        // der Speaker in Wahrheit weiterspielt. Erst nach STATUS_OFFLINE_DEBOUNCE
        // aufeinanderfolgenden Schlecht-Zyklen den Status-Downgrade ZEIGEN; ein
        // gesundes Verdikt (MIGRATED/NOT_MIGRATED) wird sofort uebernommen + resettet.
        // WICHTIG: nur das STATUS-Enum (= die Kachel) wird gehalten, NICHT cloudUrl.
        // cloudUrl folgt IMMER dem frischen Probe-Wert (bei einem Blip "") — sonst
        // saehe auto_mode/resolveEmptyCloudUrls_ einen gehaltenen Stale-Wert und der
        // Re-Claim-Skip-Guard (isEligible_ cloudUrl=="") griffe nicht -> ein zum
        // Scheitern verurteilter Re-Claim ueber das gerade blippende Telnet. Die
        // Auto-Release-Pruefung unten ueberspringt bei cloudUrl=="" ohnehin
        // (length()>0-Gate), bleibt also korrekt — exakt das Vor-Aenderungs-Verhalten.
        const bool badProbe = (upd.status == MigrationStatus::OFFLINE ||
                               upd.status == MigrationStatus::SETTLING ||
                               upd.status == MigrationStatus::UNKNOWN);
        if (badProbe) {
            if (live->offlineStreak < 255) live->offlineStreak++;
            if (live->offlineStreak >= STATUS_OFFLINE_DEBOUNCE)
                live->status = upd.status;       // bestaetigter Downgrade -> jetzt zeigen
            // sonst: vorherige (gesunde) Kachel HALTEN, Status nicht kippen.
        } else {
            live->offlineStreak = 0;
            live->status = upd.status;
        }
        live->cloudUrl     = upd.cloudUrl;       // cloudUrl folgt IMMER dem frischen Probe
        live->sourcesReady = upd.sourcesReady;
        // Auto-heal (Issue #10): einen frisch zugewiesenen synthetischen
        // accountId uebernehmen (macht den Speaker marge-eligible).
        if (upd.accountId.length() && live->accountId != upd.accountId)
            live->accountId = upd.accountId;
        // Auto-Claim: zeigt eh schon auf UNS, aber ownedByUs noch False
        // (Edge-Case nach Flash-Erase/NVS-Reset) — uebernehmen wir.
        if (!live->ownedByUs && live->cloudUrl == myBase) {
            Serial.printf("[inv] auto-claim %s (cloudUrl matches our base)\n",
                          live->name.c_str());
            live->ownedByUs = true;
            continue;
        }
        // Auto-Release NUR bei verifiziertem fremden Owner (s.o.) — dann
        // sind wir wirklich nicht mehr zustaendig und ip_failsafe darf ihn
        // beim naechsten IP-Wechsel nicht zurueck-claimen.
        if (live->ownedByUs && live->cloudUrl.length() > 0 && live->cloudUrl != myBase) {
            if (foreignOwnerVerified[i]) {
                Serial.printf("[inv] auto-release %s (cloudUrl=%s != our base %s; "
                              "owner verified)\n",
                              live->name.c_str(), live->cloudUrl.c_str(), myBase.c_str());
                live->ownedByUs = false;
            } else {
                Serial.printf("[inv] keep %s owned: cloudUrl=%s is dead/non-SixBack "
                              "(stale own base after IP change, or retired peer) — "
                              "failsafe/auto-mode will re-point it\n",
                              live->name.c_str(), live->cloudUrl.c_str());
            }
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

// Entscheidet, ob der teure /24-Active-Scan ueberhaupt etwas finden kann.
// Er lohnt nur, wenn (a) noch gar keine Speaker bekannt sind, oder (b) ein
// bekannter Speaker nach der Sync-Phase OFFLINE/UNKNOWN ist — der koennte per
// DHCP auf eine neue IP gewandert sein, die der Scan per deviceID wiederfindet.
// Sind alle bekannten Speaker erreichbar + klassifiziert (MIGRATED/
// NOT_MIGRATED/SETTLING), findet der Scan nichts Neues. Wird NACH der
// Sync-Phase ausgewertet, damit die Status frisch sind.
bool SpeakerInventory::activeScanWorthwhile_() {
    LockGuard g(*this);
    if (speakers_.empty()) return true;
    for (const auto& s : speakers_) {
        if (s.status == MigrationStatus::OFFLINE ||
            s.status == MigrationStatus::UNKNOWN) return true;
    }
    return false;
}

void SpeakerInventory::discover(bool forceActiveScan) {
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

    // Der /24-Active-Scan ist teuer: 254 HTTP-Probes (~66 s) und auf RAM-
    // knappen no-PSRAM-Targets (C5, 261 KB Heap) drueckt er free-heap an die
    // Health-Watchdog-Schwelle (-> heap_reboot). Nur laufen lassen, wenn er
    // etwas finden kann: expliziter User-Discover (forceActiveScan) ODER
    // activeScanWorthwhile_(). Sind nach der Sync-Phase alle bekannten Speaker
    // erreichbar + klassifiziert (typischer Boot-Pass: alle laengst migriert),
    // ist der Scan reine Heap-/Zeit-/Log-Spam-Verschwendung -> ueberspringen.
    if (!forceActiveScan && !activeScanWorthwhile_()) {
        Serial.printf("[inv] %u known speakers all reachable+classified — "
                      "skipping /24 active scan\n",
                      (unsigned)speakers_.size());
        saveToNVS();
        scanRunning_.store(false);
        return;
    }

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

void SpeakerInventory::refreshStatusesAsync() {
    // Manueller "Refresh status": dieselbe Probe-Arbeit wie der fruehere
    // synchrone Handler (refreshMigrationStatus + Spotify-/sources je Speaker),
    // aber ausgelagert in eine Hintergrund-Task — sonst friert die ganze WebUI
    // ein, weil refreshMigrationStatus je Speaker Telnet/BMX/Peer-Probe macht
    // UND einen stale-view-Retry mit delay() bis 30 s enthaelt (FHEM 144729 #86).
    // scanRunning_ ist *uebergreifend* fuer discover() UND diesen Refresh: der
    // compare_exchange verhindert zwei gleichzeitige Inventory-Worker; laeuft
    // schon einer, pollt das UI ohnehin /api/speakers bis isScanRunning()==false.
    bool expected = false;
    if (!scanRunning_.compare_exchange_strong(expected, true)) {
        Serial.println("[inv] refresh-status: scan/refresh already in flight, skipping spawn");
        return;
    }
    // 8192 Stack wie bg-discover: refreshMigrationStatus + Spotify-Pull fahren
    // HTTPClient/Telnet ueber alle Speaker (lwIP-Tiefe ~1.5-2 KB).
    BaseType_t r = xTaskCreate(refreshStatusTask_, "boseRefresh", 8192,
                                this, 1, nullptr);
    if (r != pdPASS) {
        // Fallback: lieber synchron als gar nicht (praktisch nie — nur bei
        // Heap-Erschoepfung). Blockiert dann zwar den Handler, aber besser als
        // ein dauerhaft gesetztes scanRunning_ ohne Worker, der es je raeumt.
        Serial.println("[inv] refresh-status: xTaskCreate failed — running synchronously");
        refreshMigrationStatus();
        for (const auto& s : list()) refreshSpotifyAccounts(s.deviceId);
        scanRunning_.store(false);
    }
}

bool SpeakerInventory::addByIp(const String& ip, ProbeFailure* failOut) {
    Speaker s;
    // Manueller Add verdient grosszuegigere Timeouts als SSDP-Background-
    // Probe — User wartet aktiv, Speaker im Standby (SoundTouch Portable)
    // kann ein paar Sekunden brauchen bis er antwortet.
    constexpr uint16_t MANUAL_CONNECT_MS = 2500;
    constexpr uint16_t MANUAL_READ_MS    = 5000;
    if (!probeIp_(ip, s, MANUAL_CONNECT_MS, MANUAL_READ_MS, failOut)) {
        // ARP-Race-Retry (2026-05-21, fix fuer fred_feuerstein P0b-Bug):
        // Direkt nach SixBack-Boot kennt der Speaker den SixBack-MAC
        // noch nicht. Sein ARP-Request kommt zwar bei SixBack an, aber
        // der ARP-Reply kann im FRITZ!Mesh die Topologie nicht ueberwinden
        // (Layer-2-Race / Repeater-Forwarding-Stutter). Symptom: TCP-SYN
        // von SixBack zu Speaker geht durch, Speaker will antworten,
        // aber sein SYN-ACK kommt nicht zurueck weil ARP-Cache fuer
        // SixBack leer + ARP-Reply kommt nicht an. Timeout nach
        // connect-Timeout (~2500ms).
        //
        // Fix: wenn der Probe wegen TIMEOUT (nicht REFUSED) failed,
        // einmal 3s warten und nochmal mit laengeren Timeouts versuchen.
        // Inzwischen hat die Mesh-Layer-2-Topology Zeit sich einzu-
        // pendeln + die ARP-Caches sich gegenseitig zu lernen.
        // Siehe [[reference-bosefix32-p0b-arp-race]] fuer tcpdump-Beweis.
        if (failOut && failOut->reason == ProbeFailReason::CONNECT_FAILED &&
            failOut->detail.indexOf("elapsed=2") >= 0) {
            Serial.printf("[probe] %s ARP-race suspected (timeout @ %ums), "
                          "retry in 3s with longer timeouts\n",
                          ip.c_str(), MANUAL_CONNECT_MS);
            delay(3000);
            if (!probeIp_(ip, s, /*connectMs=*/5000, /*readMs=*/10000, failOut)) {
                return false;
            }
            Serial.printf("[probe] %s recovered after ARP-race retry\n", ip.c_str());
        } else {
            return false;
        }
    }
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

// Holt `/listMediaServers` vom Speaker und persistiert die UUIDs in
// Speaker::mediaServerUuids. Schweigend bei Fehler (Speaker offline,
// keine DLNA-Server im LAN, etc.) — die Liste bleibt dann einfach leer.
void SpeakerInventory::refreshMediaServers(const String& deviceId) {
    String ip;
    {
        LockGuard g(*this);
        Speaker* p = findById(deviceId);
        if (!p) return;
        ip = p->ip;
    }
    if (ip.length() == 0) return;

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500);
    http.setTimeout(3000);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/listMediaServers";
    if (!http.begin(url)) return;
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String xml = http.getString();
    http.end();

    // Extrahiere alle id="UUID"-Attribute aus <media_server>-Elementen.
    // listMediaServers liefert UUIDs ohne "uuid:"-Prefix — z.B. id="fa095ecc-...".
    std::vector<String> uuids;
    int pos = 0;
    while (true) {
        int msStart = xml.indexOf("<media_server", pos);
        if (msStart < 0) break;
        int msEnd = xml.indexOf("/>", msStart);
        if (msEnd < 0) msEnd = xml.indexOf(">", msStart);
        if (msEnd < 0) break;
        String chunk = xml.substring(msStart, msEnd);
        int idIdx = chunk.indexOf("id=\"");
        if (idIdx > 0) {
            idIdx += 4;
            int idEnd = chunk.indexOf('"', idIdx);
            if (idEnd > idIdx) {
                String u = chunk.substring(idIdx, idEnd);
                if (u.length() > 0) uuids.push_back(u);
            }
        }
        pos = msEnd + 1;
    }

    LockGuard g(*this);
    if (Speaker* p = findById(deviceId)) {
        p->mediaServerUuids = uuids;
        Serial.printf("[inv][ms] %s -> %u DLNA-server UUIDs cached\n",
                      deviceId.c_str(), (unsigned)uuids.size());
    }
    saveToNVS();
}

// Holt BMX /sources vom Speaker und persistiert alle SPOTIFY-Items mit
// status="READY" in Speaker::spotifyAccounts. Diagnose-Only (Stufe 0) —
// macht linked Spotify-Accounts pro Speaker im UI sichtbar. Schema-Beleg
// (verifiziert aus Pre-Migration-Snapshots fred_feuerstein 2026-05-22):
//   <sourceItem source="SPOTIFY" sourceAccount="<user-id>" status="READY"
//       isLocal="false" multiroomallowed="true">DisplayText</sourceItem>
void SpeakerInventory::refreshSpotifyAccounts(const String& deviceId) {
    String ip;
    {
        LockGuard g(*this);
        Speaker* p = findById(deviceId);
        if (!p) return;
        ip = p->ip;
    }
    if (ip.length() == 0) return;

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500);
    http.setTimeout(3000);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/sources";
    if (!http.begin(url)) return;
    int code = http.GET();
    if (code != 200) { http.end(); return; }
    String xml = http.getString();
    http.end();

    // SPOTIFY READY-Items extrahieren. Mehrere Accounts pro Speaker moeglich
    // (fred-Snapshot 2026-05-22 hatte 2 verschiedene Spotify-User auf
    // demselben Speaker). Display-Text steht zwischen Tag-Ende und naechstem
    // <sourceItem> oder /-Selbst-Tag — bei status="READY" hat das Item den
    // Display-Namen (typisch eine Mail-Adresse) als XML-Textinhalt.
    std::vector<Speaker::SpotifyAccount> accs;
    int pos = 0;
    while (true) {
        int siStart = xml.indexOf("<sourceItem ", pos);
        if (siStart < 0) break;
        int tagEnd = xml.indexOf(">", siStart);
        if (tagEnd < 0) break;
        String tag = xml.substring(siStart, tagEnd);  // ohne ">"
        pos = tagEnd + 1;
        bool selfClose = (tag.length() > 0 && tag.endsWith("/"));
        // Nur SPOTIFY + READY weiter betrachten — UNAVAILABLE-Defaults
        // (SpotifyConnectUserName / SpotifyAlexaUserName) skippen.
        if (tag.indexOf("source=\"SPOTIFY\"") < 0) continue;
        if (tag.indexOf("status=\"READY\"")   < 0) continue;
        int accIdx = tag.indexOf("sourceAccount=\"");
        if (accIdx < 0) continue;
        int accStart = accIdx + 15;
        int accEnd = tag.indexOf("\"", accStart);
        if (accEnd <= accStart) continue;
        Speaker::SpotifyAccount sa;
        sa.sourceAccount = tag.substring(accStart, accEnd);
        // DisplayName: Textinhalt bis </sourceItem>, nur wenn nicht selfclose.
        if (!selfClose) {
            int closeIdx = xml.indexOf("</sourceItem>", pos);
            if (closeIdx > pos) {
                sa.displayName = xml.substring(pos, closeIdx);
                sa.displayName.trim();
                pos = closeIdx + 13;
            }
        }
        if (sa.sourceAccount.length() > 0) accs.push_back(sa);
    }

    LockGuard g(*this);
    if (Speaker* p = findById(deviceId)) {
        p->spotifyAccounts = accs;
        Serial.printf("[inv][spot] %s -> %u Spotify accounts (READY)\n",
                      deviceId.c_str(), (unsigned)accs.size());
        for (const auto& a : accs) {
            Serial.printf("[inv][spot]   account=%s display=%s\n",
                          a.sourceAccount.c_str(), a.displayName.c_str());
        }
    }
    saveToNVS();
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

bool SpeakerInventory::setHidden(const String& deviceId, bool hidden) {
    LockGuard g(*this);  // rekursiv — saveToNVS() nimmt ihn erneut
    for (auto& s : speakers_) {
        if (s.deviceId == deviceId) {
            if (s.hidden != hidden) {
                s.hidden = hidden;
                saveToNVS();
            }
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

void SpeakerInventory::applyOrder_(const std::vector<String>& deviceIdOrder) {
    std::vector<Speaker> reordered;
    reordered.reserve(speakers_.size());
    std::vector<bool> taken(speakers_.size(), false);
    // 1) In gewuenschter Reihenfolge uebernehmen (erstes ungenommenes Match).
    for (const String& id : deviceIdOrder) {
        for (size_t i = 0; i < speakers_.size(); ++i) {
            if (!taken[i] && speakers_[i].deviceId == id) {
                reordered.push_back(speakers_[i]);
                taken[i] = true;
                break;
            }
        }
    }
    // 2) Nicht genannte (neu entdeckte/unbekannte) in alter relativer
    //    Reihenfolge hinten anhaengen — sonst wuerde ein Reorder waehrend
    //    eines laufenden Scans frisch gefundene Speaker verlieren.
    for (size_t i = 0; i < speakers_.size(); ++i) {
        if (!taken[i]) reordered.push_back(speakers_[i]);
    }
    speakers_.swap(reordered);
}

bool SpeakerInventory::reorder(const std::vector<String>& deviceIdOrder) {
    LockGuard g(*this);  // rekursiv — saveToNVS() nimmt ihn erneut
    applyOrder_(deviceIdOrder);
    saveToNVS();
    Serial.printf("[inv] reorder applied — %u speakers\n", (unsigned)speakers_.size());
    return true;
}

} // namespace sixback

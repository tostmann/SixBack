// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "preset_store.h"
#include "nvs_helper.h"
#include "speaker_inventory.h"
#include "tunein_resolver.h"   // tuneInLogoUrl() — kanonische TuneIn-Logo-URL
#include <ArduinoJson.h>
#include "mbedtls/base64.h"
#include <vector>

namespace sixback {

namespace {

constexpr const char* NVS_NS  = "sixback-pre";
constexpr const char* NVS_KEY = "presets";

// Escaped fuer XML-Text-Inhalt UND fuer doppelt-gequotete Attribute-Werte.
// Beide Kontexte brauchen mind. & und < entkommen; im Attribut zusaetzlich ".
// Stations-Namen wie "Radio Bob & Friends" oder Stream-URLs mit Query-Parametern
// die '&' enthalten produzierten vorher ungueltiges XML — Speaker lehnt dann
// den /account/full-Sync ab, Presets bleiben leer am Geraet.
String xmlEscape_(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in.charAt(i);
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

} // anon

PresetStore& PresetStore::instance() {
    static PresetStore s;
    return s;
}

void PresetStore::initMutex_() {
    if (!mx_) mx_ = xSemaphoreCreateRecursiveMutex();
}

PresetStore::LockGuard::LockGuard(PresetStore& ps) : ps_(ps) {
    ps_.initMutex_();
    xSemaphoreTakeRecursive(ps_.mx_, portMAX_DELAY);
}

PresetStore::LockGuard::~LockGuard() {
    xSemaphoreGiveRecursive(ps_.mx_);
}

const char* presetSourceToStr(PresetSource s) {
    switch (s) {
        case PresetSource::TUNEIN:               return "TUNEIN";
        case PresetSource::LOCAL_INTERNET_RADIO: return "LOCAL_INTERNET_RADIO";
        case PresetSource::OPAQUE:               return "OPAQUE";
        default:                                  return "EMPTY";
    }
}

PresetSource presetSourceFromStr(const String& s) {
    if (s == "TUNEIN")               return PresetSource::TUNEIN;
    if (s == "LOCAL_INTERNET_RADIO") return PresetSource::LOCAL_INTERNET_RADIO;
    if (s == "OPAQUE")               return PresetSource::OPAQUE;
    return PresetSource::EMPTY;
}

void PresetStore::loadFromNVS() {
    LockGuard g(*this);
    JsonDocument doc;
    bool dataPresent = false;
    if (!nvsLoadJson(NVS_NS, NVS_KEY, doc, &dataPresent)) {
        // FHEM 144729 #153: "fresh" (kein Blob) von "Blob vorhanden aber
        // UNLESBAR" unterscheiden. Letzteres war bis 2026-07-17 komplett
        // still — der Store startete kommentarlos leer, und der naechste
        // /full-Poll konnte die Speaker-Caches wipen. Jetzt: Flag setzen
        // (handleAccountFull gated damit auf 404, /api/status zeigt
        // preset_store.load_ok=false) + laut loggen. Das Flag loescht der
        // erste erfolgreiche saveToNVS (der ersetzt den defekten Blob).
        loadFailed_ = dataPresent;
        if (dataPresent)
            Serial.println("[preset] LOAD FAILED — stored blob unreadable; "
                           "store startet leer, /full liefert 404 bis zum "
                           "ersten erfolgreichen Save (load_ok=false)");
        else
            Serial.println("[preset] no stored presets (fresh)");
        return;
    }
    loadFailed_ = false;
    speakers_.clear();
    for (JsonObject ps : doc["speakers"].as<JsonArray>()) {
        PerSpeaker s;
        s.deviceId = (const char*)ps["deviceId"];
        // Alle 6 Slots erst sauber initialisieren, sonst hängen
        // uninitialisierte uint8_t-Werte in slot/source.
        for (int i = 0; i < 6; ++i) {
            s.slots[i].slot   = i + 1;
            s.slots[i].source = PresetSource::EMPTY;
        }
        for (JsonObject pj : ps["presets"].as<JsonArray>()) {
            uint8_t slot = pj["slot"].as<uint8_t>();
            if (slot < 1 || slot > 6) continue;
            Preset& p     = s.slots[slot - 1];
            p.slot        = slot;
            p.source      = presetSourceFromStr(String((const char*)pj["source"]));
            p.name        = (const char*)(pj["name"]      | "");
            p.stationId   = (const char*)(pj["stationId"] | "");
            p.streamUrl   = (const char*)(pj["streamUrl"] | "");
            // Gegenstueck zur Sparmassnahme in saveToNVS(): ein FEHLENDES
            // imageUrl-Feld heisst bei TUNEIN "war die kanonische Logo-URL"
            // -> rekonstruieren. Ein VORHANDENES Feld (auch ein leeres) wird
            // unveraendert uebernommen, damit "bewusst kein Cover" und alle
            // Daten aus Firmwares VOR diesem Fix unveraendert bleiben.
            if (pj["imageUrl"].isNull() && p.source == PresetSource::TUNEIN) {
                p.imageUrl = tuneInLogoUrl(p.stationId);
            } else {
                p.imageUrl = (const char*)(pj["imageUrl"] | "");
            }
            p.rawContentItem   = (const char*)(pj["rawContentItem"]   | "");
            p.opaqueSourceName = (const char*)(pj["opaqueSourceName"] | "");
        }
        speakers_.push_back(s);
    }
    Serial.printf("[preset] loaded presets for %u speakers\n",
                  (unsigned)speakers_.size());
}

bool PresetStore::saveToNVS() {
    LockGuard g(*this);
    JsonDocument doc;
    JsonArray arr = doc["speakers"].to<JsonArray>();
    for (auto& s : speakers_) {
        // Komplett leere Eintraege nicht persistieren: semantisch identisch
        // zu "absent" (buildDevicePresets_ liefert so oder so kein <presets>),
        // aber sie akkumulieren sonst als Dauer-Cruft im Blob (jeder je
        // gesehene/geloeschte Speaker bliebe fuer immer drin). Spotify-
        // Bindings liegen separat in sixback-spot und sind nicht betroffen.
        bool any = false;
        for (int i = 0; i < 6; ++i) {
            if (s.slots[i].source != PresetSource::EMPTY) { any = true; break; }
        }
        if (!any) continue;
        JsonObject ps = arr.add<JsonObject>();
        ps["deviceId"] = s.deviceId;
        JsonArray pa  = ps["presets"].to<JsonArray>();
        for (int i = 0; i < 6; ++i) {
            const Preset& p = s.slots[i];
            if (p.source == PresetSource::EMPTY) continue;
            JsonObject pj = pa.add<JsonObject>();
            pj["slot"]      = i + 1;
            pj["source"]    = presetSourceToStr(p.source);
            pj["name"]      = p.name;
            pj["stationId"] = p.stationId;
            // --- NVS-Sparmassnahme fuer TUNEIN (Lab-Messung 2026-07-30) -----
            // Die NVS-Partition hat 630 Entries fuer ALLES (WiFi, Inventar,
            // Store, Spotify, ...). Gemessen: ein TUNEIN-Preset MIT beiden URLs
            // kostet ~6 Entries, ohne sie ~1 — die Store-Kante lag bei
            // 5 Speakern (volle Presets) gegenueber >22 (schlanke).
            //
            // streamUrl ist bei TUNEIN funktional bedeutungslos: jeder
            // Konsument verzweigt vorher auf stationId und baut die location
            // selbst ("/v1/playback/station/" + stationId) — buildDevicePresets_
            // hier, der Zwilling in bose_endpoints.cpp, die Push-Pipeline, und
            // im HW-Preset-Vergleich liegt der streamUrl-Vergleich
            // ausschliesslich im LOCAL_INTERNET_RADIO-Zweig.
            // NUR die Persistenz wird schlank — exportJson() bleibt absichtlich
            // vollstaendig, damit ein Export/Backup selbsttragend ist.
            //
            // AUSNAHME Tunnel-Sentinels (sspot{N} Spotify, sstrm{N} Custom-
            // Stream): das sind TUNEIN-Presets, deren streamUrl NICHT aus der
            // stationId rekonstruierbar ist. Der TuneIn-Resolver
            // (findPresetOverride) liefert sie beim HW-Tastendruck aus; bei
            // sstrm ist die gespeicherte URL sogar die einzige Kopie. Sie
            // wegzulassen killt den Tunnel nach dem naechsten Reboot
            // (HW-reproduziert 2026-07-30). Kosten: ~5 Entries je Tunnel-Slot,
            // hoechstens 6 pro Speaker und nur bei Nutzern, die Tunnel anlegen.
            const bool isTunnelSentinel = p.stationId.startsWith("sspot")
                                       || p.stationId.startsWith("sstrm");
            if (p.source != PresetSource::TUNEIN || isTunnelSentinel) {
                pj["streamUrl"] = p.streamUrl;
            }
            // imageUrl WIRD gebraucht (<containerArt>), ist bei TUNEIN aber
            // meist exakt die kanonische Logo-URL zur stationId. Nur die wird
            // weggelassen und beim Laden rekonstruiert — jede abweichende URL
            // (eigenes Cover, anderes Format, Cache-Buster) bleibt gespeichert.
            // "Bewusst leer" bleibt erhalten: ein leeres Feld wird geschrieben,
            // nur ein FEHLENDES gilt beim Laden als "kanonisch rekonstruieren".
            if (!(p.source == PresetSource::TUNEIN &&
                  p.imageUrl.length() > 0 &&
                  p.imageUrl == tuneInLogoUrl(p.stationId))) {
                pj["imageUrl"] = p.imageUrl;
            }
            if (p.source == PresetSource::OPAQUE) {
                pj["rawContentItem"]   = p.rawContentItem;
                pj["opaqueSourceName"] = p.opaqueSourceName;
            }
        }
    }
    // FHEM 144729 #153: ein bei Heap-Knappheit ueberlaufenes JsonDocument
    // (ArduinoJson dropt Nodes still) wuerde als VALIDES Teil-JSON committed
    // und verloere die hinteren Speaker-Slices dauerhaft. Dann lieber gar
    // nicht schreiben — der alte NVS-Stand bleibt intakt, RAM ist weiterhin
    // vollstaendig, der naechste Save (mit mehr Heap) heilt.
    if (doc.overflowed()) {
        ++saveFails_;
        ++saveHeapAborts_;          // transient: NVS intakt, naechster Save heilt
        Serial.println("[preset] saveToNVS ABORT: JsonDocument overflowed "
                       "(heap zu knapp) — NVS-Stand bleibt unangetastet");
        return false;
    }
    bool ok = nvsSaveJsonWithCleanup(NVS_NS, NVS_KEY, doc);
    if (ok) {
        // Erfolgreicher Save ersetzt einen ggf. defekten Boot-Blob ->
        // Load-Fail-Zustand ist damit geheilt, /full darf wieder servieren.
        loadFailed_ = false;
        ++saveOkCount_;             // Generation fuer den Auto-Import-Backoff
    } else {
        // Zentrale Zaehlung fuer ALLE Caller — clear()/syncToGroup()
        // ignorierten das Ergebnis bis 2026-07-17 komplett (silent loss).
        ++saveFails_;
        ++saveNvsFails_;            // echter Schreibfehler = Kapazitaetskante
        Serial.printf("[preset] saveToNVS FAILED (nvs write, #%u)\n",
                      (unsigned)saveNvsFails_);
    }
    return ok;
}

size_t PresetStore::speakerCount() {
    LockGuard g(*this);
    return speakers_.size();
}

PresetStore::PerSpeaker* PresetStore::findOrCreate_(const String& deviceId) {
    if (auto* p = find_(deviceId)) return p;
    PerSpeaker s;
    s.deviceId = deviceId;
    for (int i = 0; i < 6; ++i) {
        s.slots[i].slot = i + 1;
        s.slots[i].source = PresetSource::EMPTY;
    }
    speakers_.push_back(s);
    return &speakers_.back();
}

PresetStore::PerSpeaker* PresetStore::find_(const String& deviceId) {
    for (auto& s : speakers_) {
        if (s.deviceId == deviceId) return &s;
    }
    return nullptr;
}

std::vector<Preset> PresetStore::getForSpeaker(const String& deviceId) {
    LockGuard g(*this);
    std::vector<Preset> out;
    auto* s = find_(deviceId);
    for (int i = 0; i < 6; ++i) {
        Preset p;
        p.slot   = i + 1;
        p.source = PresetSource::EMPTY;
        if (s) {
            p = s->slots[i];
            // Defensiv: falls NVS-State korrupt war oder eine Migration
            // den slot-Wert verworfen hat, hier korrigieren.
            p.slot = i + 1;
        }
        out.push_back(p);
    }
    return out;
}

Preset PresetStore::get(const String& deviceId, uint8_t slot) {
    LockGuard g(*this);
    Preset p; p.slot = slot; p.source = PresetSource::EMPTY;
    if (slot < 1 || slot > 6) return p;
    auto* s = find_(deviceId);
    if (!s) return p;
    return s->slots[slot - 1];
}

bool PresetStore::set(const String& deviceId, const Preset& p) {
    LockGuard g(*this);
    if (p.slot < 1 || p.slot > 6) return false;
    auto* s = findOrCreate_(deviceId);
    s->slots[p.slot - 1] = p;
    return saveToNVS();
}

bool PresetStore::setSlots(const String& deviceId, const std::vector<Preset>& presets) {
    if (presets.empty()) return false;
    LockGuard g(*this);
    auto* s = findOrCreate_(deviceId);
    bool changed = false;
    for (const auto& p : presets) {
        if (p.slot < 1 || p.slot > 6) continue;
        s->slots[p.slot - 1]      = p;
        s->slots[p.slot - 1].slot = p.slot;  // defensiv re-stamp slot
        changed = true;
    }
    if (changed) return saveToNVS();
    return false;
}

bool PresetStore::clear(const String& deviceId, uint8_t slot) {
    LockGuard g(*this);
    if (slot < 1 || slot > 6) return false;
    auto* s = find_(deviceId);
    if (!s) return false;
    Preset& p = s->slots[slot - 1];
    p.slot   = slot;
    p.source = PresetSource::EMPTY;
    p.name = ""; p.stationId = ""; p.streamUrl = ""; p.imageUrl = "";
    p.rawContentItem = ""; p.opaqueSourceName = "";
    saveToNVS();
    return true;
}

int PresetStore::syncToGroup(const String& sourceDeviceId,
                              const std::vector<String>& targetDeviceIds) {
    LockGuard g(*this);
    auto* src = find_(sourceDeviceId);
    if (!src) return 0;
    int n = 0;
    for (const auto& tgtId : targetDeviceIds) {
        if (tgtId == sourceDeviceId) continue;
        auto* tgt = findOrCreate_(tgtId);
        for (int i = 0; i < 6; ++i) {
            tgt->slots[i] = src->slots[i];
            tgt->slots[i].slot = i + 1;
        }
        ++n;
    }
    if (n > 0) saveToNVS();
    return n;
}

void PresetStore::exportJson(JsonDocument& out) {
    LockGuard g(*this);
    JsonArray arr = out["speakers"].to<JsonArray>();
    for (auto& s : speakers_) {
        JsonObject ps = arr.add<JsonObject>();
        ps["deviceId"] = s.deviceId;
        JsonArray pa  = ps["presets"].to<JsonArray>();
        for (int i = 0; i < 6; ++i) {
            const Preset& p = s.slots[i];
            JsonObject pj = pa.add<JsonObject>();
            pj["slot"]      = i + 1;
            pj["source"]    = presetSourceToStr(p.source);
            pj["name"]      = p.name;
            pj["stationId"] = p.stationId;
            pj["streamUrl"] = p.streamUrl;
            pj["imageUrl"]  = p.imageUrl;
            if (p.source == PresetSource::OPAQUE) {
                pj["rawContentItem"]   = p.rawContentItem;
                pj["opaqueSourceName"] = p.opaqueSourceName;
            }
        }
    }
}

bool PresetStore::importJson(JsonDocument& in) {
    LockGuard g(*this);
    speakers_.clear();
    for (JsonObject ps : in["speakers"].as<JsonArray>()) {
        PerSpeaker s;
        s.deviceId = (const char*)ps["deviceId"];
        for (JsonObject pj : ps["presets"].as<JsonArray>()) {
            uint8_t slot = pj["slot"].as<uint8_t>();
            if (slot < 1 || slot > 6) continue;
            Preset& p     = s.slots[slot - 1];
            p.slot        = slot;
            p.source      = presetSourceFromStr(String((const char*)pj["source"]));
            p.name        = (const char*)(pj["name"]      | "");
            p.stationId   = (const char*)(pj["stationId"] | "");
            p.streamUrl   = (const char*)(pj["streamUrl"] | "");
            p.imageUrl    = (const char*)(pj["imageUrl"]  | "");
            p.rawContentItem   = (const char*)(pj["rawContentItem"]   | "");
            p.opaqueSourceName = (const char*)(pj["opaqueSourceName"] | "");
        }
        speakers_.push_back(s);
    }
    saveToNVS();
    return true;
}

bool PresetStore::findByStationId(const String& stationId, Preset& out) {
    LockGuard g(*this);
    for (auto& s : speakers_) {
        for (int i = 0; i < 6; ++i) {
            const Preset& p = s.slots[i];
            if (p.source == PresetSource::EMPTY) continue;
            if (p.stationId == stationId) {
                out = p;
                return true;
            }
        }
    }
    return false;
}

bool PresetStore::hasAnyFor(const String& deviceId) {
    LockGuard g(*this);
    auto* s = find_(deviceId);
    if (!s) return false;
    for (int i = 0; i < 6; ++i) {
        if (s->slots[i].source != PresetSource::EMPTY) return true;
    }
    return false;
}

String escapeXml(const String& in) { return xmlEscape_(in); }

String unescapeXml(const String& in) {
    String out = in;
    out.replace("&lt;", "<");
    out.replace("&gt;", ">");
    out.replace("&quot;", "\"");
    out.replace("&apos;", "'");
    out.replace("&amp;", "&");   // zuletzt: sonst wird z.B. "&amp;lt;" doppelt dekodiert
    return out;
}

String orionStationLocation(const String& streamUrl, const String& name,
                            const String& imageUrl) {
    // Envelope-Form wie der echte svc-bmx-adapter-orion / gmuth station.php.
    JsonDocument d;
    d["streamUrl"]  = streamUrl;
    d["name"]       = name;
    d["imageUrl"]   = imageUrl;
    d["streamType"] = "liveRadio";
    d["isRealtime"] = true;
    String json;
    serializeJson(d, json);

    size_t need = 0;
    mbedtls_base64_encode(nullptr, 0, &need,
        (const unsigned char*)json.c_str(), json.length());
    std::vector<unsigned char> buf(need + 1, 0);
    size_t olen = 0;
    if (mbedtls_base64_encode(buf.data(), buf.size(), &olen,
            (const unsigned char*)json.c_str(), json.length()) != 0) {
        return String();   // Puffer zu klein — kann mit need+1 nicht passieren
    }
    String b64((const char*)buf.data());   // mbedtls null-terminiert
    // url-safe: '+' und '/' tauschen; '='-Padding bleibt (im Query gueltig,
    // handleOrionStation tauscht '-_'->'+/' zurueck, Padding intakt).
    b64.replace('+', '-');
    b64.replace('/', '_');
    return String("/station?data=") + b64;
}

String PresetStore::toBoseXml(const String& deviceId) {
    LockGuard g(*this);
    // Format aus Pre-Migration-Snapshot der Bose Cloud:
    //   <presets><preset id="N"><ContentItem source="TUNEIN" type="stationurl"
    //   location="/v1/playback/station/sXXXXX" sourceAccount="" isPresetable="true">
    //   <itemName>NAME</itemName></ContentItem></preset>...</presets>
    String out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<presets>";
    auto* s = find_(deviceId);
    if (s) {
        for (int i = 0; i < 6; ++i) {
            const Preset& p = s->slots[i];
            if (p.source == PresetSource::EMPTY) continue;
            if (p.source == PresetSource::OPAQUE) {
                // Passthrough: das ContentItem-XML wie wir es vom Speaker
                // gesehen haben 1:1 einbetten. Speaker erkennt sein eigenes
                // Preset wieder, kann DLNA/Bluetooth/etc. direkt ansprechen.
                if (p.rawContentItem.length() > 0) {
                    out += "<preset id=\"";
                    out += String(p.slot);
                    out += "\">";
                    out += p.rawContentItem;
                    out += "</preset>";
                }
                continue;
            }
            out += "<preset id=\"";
            out += String(p.slot);
            out += "\"><ContentItem source=\"";
            out += presetSourceToStr(p.source);  // enum-Konst — safe ohne Escape
            // type="stationurl" fuer BEIDE adapter-aufgeloesten Quellen:
            // TUNEIN (location /v1/playback/station/<sid>) und
            // LOCAL_INTERNET_RADIO (location /station?data=… via ORION-Adapter).
            // LIR mit type="url"+roher Stream-URL spielt der Speaker NICHT;
            // mit stationurl+ORION-location schon (on-device 2026-06-03, Emma).
            out += "\" type=\"";
            out += "stationurl";
            out += "\" location=\"";
            if (p.source == PresetSource::TUNEIN) {
                out += "/v1/playback/station/";
                out += xmlEscape_(p.stationId);
            } else {
                // LOCAL_INTERNET_RADIO ueber den nativen ORION-Adapter statt
                // roher Stream-URL (die spielt der Speaker nicht). base64 ist
                // url-safe -> kein XML-Escape noetig.
                out += orionStationLocation(p.streamUrl, p.name, p.imageUrl);
            }
            // sourceAccount muss zum /sources-Eintrag am Speaker passen. Bei
            // TUNEIN ist das "TuneIn" (so kommt es auch im Bose-Werks-Cloud-
            // Sync). Leerer sourceAccount fuehrt am Speaker zu HTTP 500
            // "UNKNOWN_SOURCE_ERROR" beim /select.
            out += "\" sourceAccount=\"";
            out += (p.source == PresetSource::TUNEIN) ? "TuneIn" : "";
            out += "\" isPresetable=\"true\"><itemName>";
            out += xmlEscape_(p.name);
            out += "</itemName>";
            if (p.imageUrl.length() > 0) {
                out += "<containerArt>";
                out += xmlEscape_(p.imageUrl);
                out += "</containerArt>";
            }
            out += "</ContentItem></preset>";
        }
    }
    out += "</presets>";
    return out;
}

} // namespace sixback

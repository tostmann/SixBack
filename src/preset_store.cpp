// SPDX-License-Identifier: GPL-3.0-or-later
#include "preset_store.h"
#include "nvs_helper.h"
#include "speaker_inventory.h"

namespace bosefix {

namespace {

constexpr const char* NVS_NS  = "bosefix-pre";
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
        default:                                  return "EMPTY";
    }
}

PresetSource presetSourceFromStr(const String& s) {
    if (s == "TUNEIN")               return PresetSource::TUNEIN;
    if (s == "LOCAL_INTERNET_RADIO") return PresetSource::LOCAL_INTERNET_RADIO;
    return PresetSource::EMPTY;
}

void PresetStore::loadFromNVS() {
    LockGuard g(*this);
    JsonDocument doc;
    if (!nvsLoadJson(NVS_NS, NVS_KEY, doc)) return;
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
            p.imageUrl    = (const char*)(pj["imageUrl"]  | "");
        }
        speakers_.push_back(s);
    }
    Serial.printf("[preset] loaded presets for %u speakers\n",
                  (unsigned)speakers_.size());
}

void PresetStore::saveToNVS() {
    LockGuard g(*this);
    JsonDocument doc;
    JsonArray arr = doc["speakers"].to<JsonArray>();
    for (auto& s : speakers_) {
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
            pj["streamUrl"] = p.streamUrl;
            pj["imageUrl"]  = p.imageUrl;
        }
    }
    nvsSaveJson(NVS_NS, NVS_KEY, doc);
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
    saveToNVS();
    return true;
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
    if (changed) saveToNVS();
    return changed;
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
            out += "<preset id=\"";
            out += String(p.slot);
            out += "\"><ContentItem source=\"";
            out += presetSourceToStr(p.source);  // enum-Konst — safe ohne Escape
            out += "\" type=\"stationurl\" location=\"";
            if (p.source == PresetSource::TUNEIN) {
                out += "/v1/playback/station/";
                out += xmlEscape_(p.stationId);
            } else {
                out += xmlEscape_(p.streamUrl);
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

} // namespace bosefix

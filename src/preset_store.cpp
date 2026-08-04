// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "preset_store.h"
#include "nvs_helper.h"
#include "speaker_inventory.h"
#include "tunein_resolver.h"   // tuneInLogoUrl() — kanonische TuneIn-Logo-URL
#include <ArduinoJson.h>
#include <Preferences.h>
#include <nvs.h>
#include "mbedtls/base64.h"
#include <vector>

namespace sixback {

namespace {

constexpr const char* NVS_NS  = "sixback-pre";
// Legacy-Gesamtblob (<= v0.8.39): ALLE Speaker in EINEM Key. Wird seit der
// Per-Speaker-Umstellung nur noch als Migrationsquelle gelesen und nach
// erfolgreicher Migration geloescht. ACHTUNG Downgrade-Kaskade: eine
// Firmware <= v0.8.39 sieht nach der Migration einen leeren Store und
// re-importiert beim naechsten Seitenaufbau von den Boxen (Presets auf den
// Geraeten bleiben unberuehrt) — bewusster Trade-off, dokumentiert im
// Release.
constexpr const char* LEGACY_KEY = "presets";

// true fuer den Legacy-Key und seine A/B-Geschwister — die duerfen bei der
// Namespace-Enumeration nie als deviceId fehlinterpretiert werden.
bool isLegacyKeyName_(const char* k) {
    return strcmp(k, "presets")   == 0 || strcmp(k, "presets~")  == 0 ||
           strcmp(k, "presets#0") == 0 || strcmp(k, "presets#1") == 0;
}

// "<id>", "<id>~", "<id>#0", "<id>#1" -> "<id>" (A/B-Suffixe des nvs_helper).
String baseIdFromKey_(const char* k) {
    String s(k);
    if (s.endsWith("~")) return s.substring(0, s.length() - 1);
    if (s.length() >= 2 && s.charAt(s.length() - 2) == '#') {
        const char c = s.charAt(s.length() - 1);
        if (c == '0' || c == '1') return s.substring(0, s.length() - 2);
    }
    return s;
}

// Alle Per-Speaker-Basis-Ids im Namespace (Blob-Slices UND Gen-Keys, damit
// ein Slice, der nur im Spare-Slot "<id>~" liegt, gefunden wird). Dedupe
// linear — Flotten sind <= ~20 Geraete.
void listPerSpeakerIds_(std::vector<String>& out) {
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", NVS_NS, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        if (!isLegacyKeyName_(info.key)) {
            String base = baseIdFromKey_(info.key);
            bool seen = false;
            for (auto& b : out) { if (b == base) { seen = true; break; } }
            if (!seen && base.length()) out.push_back(base);
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
}

// Loescht einen Basis-Key samt A/B-Geschwistern. isKey-Guard, damit kein
// nvs_erase_key-NOT_FOUND-E-Log entsteht (die Zeilen haben in freds Serial-
// Mitschnitt msg1367457 fuer Verwirrung gesorgt; sie sind harmlos, aber
// vermeidbar). Reines Erase = die einzige Operation, die an der vollen
// Partition Platz SCHAFFT, ohne vorher schreiben zu muessen.
void eraseStoreKeys_(const char* baseKey) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    const String k1 = String(baseKey) + "~";
    const String g0 = String(baseKey) + "#0";
    const String g1 = String(baseKey) + "#1";
    if (p.isKey(baseKey))    p.remove(baseKey);
    if (p.isKey(k1.c_str())) p.remove(k1.c_str());
    if (p.isKey(g0.c_str())) p.remove(g0.c_str());
    if (p.isKey(g1.c_str())) p.remove(g1.c_str());
    p.end();
}

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

// Ein Speaker-Slice ({deviceId, presets:[...]}) aus NVS-JSON. Gleiche Form
// als Array-Element im Legacy-Gesamtblob und als Root-Objekt eines
// Per-Speaker-Keys — deviceId steht redundant im Slice (self-describing,
// ~25 B), damit Export/Debug ohne Key-Kontext lesbar bleiben.
void PresetStore::parseSlice_(JsonObject ps, PerSpeaker& s) {
    s.deviceId = (const char*)(ps["deviceId"] | "");
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
        // Gegenstueck zur Sparmassnahme in buildSliceDoc_(): ein FEHLENDES
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
}

void PresetStore::loadFromNVS() {
    LockGuard g(*this);
    speakers_.clear();
    loadFailedIds_.clear();
    legacyLoadFailed_ = false;

    // 1) Legacy-Gesamtblob (<= v0.8.39), falls noch vorhanden, als Basis
    //    einlesen. Per-Speaker-Slices (Schritt 2) ueberschreiben ihn je
    //    Device — nach einer unterbrochenen Migration existieren beide, und
    //    die Slices sind dann der juengere Stand.
    bool legacyReadable = false;
    bool legacyPresent  = false;
    {
        JsonDocument doc;
        if (nvsLoadJson(NVS_NS, LEGACY_KEY, doc, &legacyPresent)) {
            legacyReadable = true;
            for (JsonObject ps : doc["speakers"].as<JsonArray>()) {
                PerSpeaker s;
                parseSlice_(ps, s);
                if (s.deviceId.length()) speakers_.push_back(s);
            }
        } else if (legacyPresent) {
            // FHEM 144729 #153: Blob liegt da, ist aber unlesbar. NICHT
            // loeschen (letzte Kopie/Forensik) und NICHT migrieren — /full
            // bleibt via load_ok=false gated (404, Speaker behalten ihre
            // Caches). Anders als beim Gesamtblob-Format heilt ein
            // spaeterer Save diesen Zustand nicht mehr (er ersetzt den
            // defekten Blob nicht laenger) — das Gate steht bewusst bis
            // zum naechsten Boot; nvs_flash_init raeumt echte
            // Inkonsistenzen dort ohnehin weg.
            legacyLoadFailed_ = true;
            Serial.println("[preset] LOAD FAILED — legacy blob unreadable; "
                           "/full liefert 404 (load_ok=false), Blob bleibt "
                           "fuer Forensik liegen");
        }
    }

    // 2) Per-Speaker-Slices (v0.8.40+).
    std::vector<String> sliceIds;
    listPerSpeakerIds_(sliceIds);
    std::vector<String> loadedSliceIds;
    for (auto& id : sliceIds) {
        JsonDocument doc;
        bool present = false;
        if (nvsLoadJson(NVS_NS, id.c_str(), doc, &present)) {
            PerSpeaker s;
            JsonObject ps = doc.as<JsonObject>();
            parseSlice_(ps, s);
            if (!s.deviceId.length()) s.deviceId = id;   // defensiv: Key gilt
            auto* existing = find_(s.deviceId);
            if (existing) *existing = s; else speakers_.push_back(s);
            loadedSliceIds.push_back(id);
        } else if (present) {
            // Slice vorhanden aber unlesbar (beide A/B-Slots defekt — nach
            // Readback-Verify + Slot-Fallback ein Randfall). Gate wie beim
            // Legacy-Fall; heilt, sobald genau DIESER Slice wieder
            // erfolgreich geschrieben wird (healLoadFail_).
            loadFailedIds_.push_back(id);
            Serial.printf("[preset] LOAD FAILED slice %s — unlesbar, /full "
                          "gated (load_ok=false)\n", id.c_str());
        }
    }
    loadFailed_ = legacyLoadFailed_ || !loadFailedIds_.empty();

    if (!legacyPresent && sliceIds.empty()) {
        Serial.println("[preset] no stored presets (fresh)");
        return;
    }
    Serial.printf("[preset] loaded presets for %u speakers (%u per-speaker "
                  "slice(s)%s)\n",
                  (unsigned)speakers_.size(), (unsigned)loadedSliceIds.size(),
                  legacyReadable ? ", legacy blob pending migration" : "");

    // 3) Einmal-Migration Gesamtblob -> Per-Speaker-Keys; nur wenn der
    //    Legacy-Blob LESBAR war (sonst gibt es nichts zu retten).
    if (legacyReadable) migrateLegacyBlob_(loadedSliceIds);
}

// Heilt das Load-Fail-Gate fuer einen Slice, der soeben erfolgreich
// geschrieben (oder als leer geloescht) wurde — der defekte Blob unter
// diesem Key ist damit ersetzt bzw. weg.
void PresetStore::healLoadFail_(const String& deviceId) {
    for (size_t i = 0; i < loadFailedIds_.size(); ++i) {
        if (loadFailedIds_[i] == deviceId) {
            loadFailedIds_.erase(loadFailedIds_.begin() + i);
            break;
        }
    }
    loadFailed_ = legacyLoadFailed_ || !loadFailedIds_.empty();
}

// Einmal-Migration: jeden Speaker aus dem Legacy-Gesamtblob als eigenen
// Key persistieren, danach den Gesamtblob loeschen.
//
// Phase A schreibt die Slices NEBEN den Legacy-Blob (verlustfrei, braucht
// aber Platz fuer beides). Scheitert das an der Platz-Kante — exakt der
// Zustand, in dem das Geraet ohnehin nichts mehr persistieren kann
// (FHEM 144729, NOT_ENOUGH_SPACE bei 9 Boxen) — folgt Phase B: Legacy-Blob
// ZUERST loeschen (reines Erase schafft Platz ohne Write; der volle Stand
// liegt im RAM), dann die restlichen Slices schreiben. Restrisiko Phase B:
// Stromverlust im <1-s-Fenster zwischen Erase und letztem Slice-Write
// verliert die noch ungeschriebenen Slices — abgewogen gegen "Store
// dauerhaft schreibgesperrt" der bessere Deal, und nur auf Geraeten, die
// Phase A nicht schaffen.
void PresetStore::migrateLegacyBlob_(const std::vector<String>& haveSlice) {
    std::vector<String> pending;
    for (auto& s : speakers_) {
        bool have = false;
        for (auto& id : haveSlice) { if (id == s.deviceId) { have = true; break; } }
        if (have) continue;
        bool any = false;
        for (int i = 0; i < 6; ++i) {
            if (s.slots[i].source != PresetSource::EMPTY) { any = true; break; }
        }
        if (!any) continue;   // Leer-Slices nie materialisieren (Anti-Cruft)
        if (s.deviceId.length() == 0 || s.deviceId.length() > 15) {
            // Kann nie NVS-Key werden (echte deviceIds sind 12-hex-MACs).
            // Sicherheit geht vor: Legacy-Blob NICHT anfassen, Migration
            // beim naechsten Boot erneut versuchen.
            Serial.printf("[preset] migrate ABORT: deviceId '%s' ist kein "
                          "gueltiger NVS-Key — legacy blob bleibt\n",
                          s.deviceId.c_str());
            return;
        }
        pending.push_back(s.deviceId);
    }
    if (pending.empty()) {
        // Alle Speaker haben schon Slices (Migration war fast durch) —
        // nur noch den Gesamtblob wegraeumen.
        eraseStoreKeys_(LEGACY_KEY);
        Serial.println("[preset] migration: alle Slices vorhanden, legacy "
                       "blob entfernt");
        return;
    }
    // Fail-Zaehler sichern: ein erwarteter Phase-A-Fail an der Kante, den
    // Phase B heilt, soll im /api/status nicht als Feld-Problem auftauchen
    // (fred wuerde ihn melden). Bleibt die Migration unvollstaendig, bleiben
    // die Zaehler stehen — dann sind es echte Fails.
    const uint32_t preFails    = saveFails_;
    const uint32_t preNvsFails = saveNvsFails_;
    std::vector<String> failed;
    for (auto& id : pending) {
        if (!saveSpeakerToNVS_(id)) failed.push_back(id);
    }
    if (failed.empty()) {
        eraseStoreKeys_(LEGACY_KEY);
        saveFails_ = preFails; saveNvsFails_ = preNvsFails;
        Serial.printf("[preset] migrated %u speaker(s) to per-speaker keys, "
                      "legacy blob removed\n", (unsigned)pending.size());
        return;
    }
    Serial.printf("[preset] migrate phase A: %u/%u slice(s) passten nicht "
                  "NEBEN den legacy blob — Druck-Migration (Blob zuerst "
                  "loeschen, Stand liegt vollstaendig im RAM)\n",
                  (unsigned)failed.size(), (unsigned)pending.size());
    eraseStoreKeys_(LEGACY_KEY);
    size_t still = 0;
    for (auto& id : failed) {
        if (!saveSpeakerToNVS_(id)) {
            ++still;
            Serial.printf("[preset] migrate FAIL %s — Slice bleibt RAM-only "
                          "bis Platz frei wird\n", id.c_str());
        }
    }
    if (still == 0) {
        saveFails_ = preFails; saveNvsFails_ = preNvsFails;
        Serial.printf("[preset] migrated %u speaker(s) via Druck-Migration, "
                      "legacy blob removed\n", (unsigned)pending.size());
    } else {
        Serial.printf("[preset] migration UNVOLLSTAENDIG: %u Slice(s) "
                      "RAM-only (NVS weiterhin zu voll)\n", (unsigned)still);
    }
}

// Serialisiert EINEN Speaker als Slice-Dokument {deviceId, presets:[...]}.
// Enthaelt die komplette NVS-Sparlogik (TUNEIN-Slimming) — Export/Backup
// (exportJson) bleibt davon unberuehrt und absichtlich vollstaendig.
void PresetStore::buildSliceDoc_(const PerSpeaker& s, JsonDocument& doc) {
    doc["deviceId"] = s.deviceId;
    JsonArray pa = doc["presets"].to<JsonArray>();
    {
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
}

// Persistiert EINEN Speaker (Caller haelt den Lock). Leerer/fehlender Slice
// -> Key wird GELOESCHT statt ein Leer-Blob geschrieben: gleiche Semantik
// wie die alte Anti-Cruft-Regel im Gesamtblob ("absent"), gibt aber Entries
// FREI — an der vollen Partition die einzige Operation, die das kann
// (144729-Catch-22: jede andere Verkleinerung war selbst ein Write).
bool PresetStore::saveSpeakerToNVS_(const String& deviceId) {
    if (deviceId.length() == 0 || deviceId.length() > 15) {
        // NVS-Keys enden bei 15 Zeichen; echte deviceIds (12-hex-MAC) passen
        // immer. Alles andere ist ein Programmierfehler -> laut + zaehlen.
        ++saveFails_;
        ++saveNvsFails_;
        Serial.printf("[preset] saveSpeaker REJECT: deviceId '%s' ist kein "
                      "gueltiger NVS-Key\n", deviceId.c_str());
        return false;
    }
    auto* s = find_(deviceId);
    bool any = false;
    if (s) {
        for (int i = 0; i < 6; ++i) {
            if (s->slots[i].source != PresetSource::EMPTY) { any = true; break; }
        }
    }
    if (!any) {
        eraseStoreKeys_(deviceId.c_str());
        ++saveOkCount_;             // Zustand hat sich real geaendert/gebessert
        healLoadFail_(deviceId);
        return true;
    }
    JsonDocument doc;
    buildSliceDoc_(*s, doc);
    // FHEM 144729 #153: ein bei Heap-Knappheit ueberlaufenes JsonDocument
    // (ArduinoJson dropt Nodes still) wuerde als VALIDES Teil-JSON committed.
    // Bei einem Einzel-Slice (~1 KB) praktisch unerreichbar, aber der Check
    // kostet nichts — lieber gar nicht schreiben, der naechste Save heilt.
    if (doc.overflowed()) {
        ++saveFails_;
        ++saveHeapAborts_;          // transient: NVS intakt, naechster Save heilt
        Serial.println("[preset] saveSpeaker ABORT: JsonDocument overflowed "
                       "(heap zu knapp) — NVS-Stand bleibt unangetastet");
        return false;
    }
    bool ok = nvsSaveJsonWithCleanup(NVS_NS, deviceId.c_str(), doc);
    if (ok) {
        // Erfolgreicher Slice-Save ersetzt einen ggf. defekten Blob unter
        // GENAU diesem Key -> dessen Load-Fail-Gate ist geheilt.
        healLoadFail_(deviceId);
        ++saveOkCount_;             // Generation fuer den Auto-Import-Backoff
    } else {
        // Zentrale Zaehlung fuer ALLE Caller — clear()/syncToGroup()
        // ignorierten das Ergebnis bis 2026-07-17 komplett (silent loss).
        ++saveFails_;
        ++saveNvsFails_;            // echter Schreibfehler = Kapazitaetskante
        Serial.printf("[preset] saveSpeaker FAILED (nvs write, #%u) dev=%s\n",
                      (unsigned)saveNvsFails_, deviceId.c_str());
    }
    return ok;
}

bool PresetStore::saveToNVS() {
    LockGuard g(*this);
    // Vollsweep (Import/Restore): jeden Slice einzeln schreiben, danach
    // Keys verwaister Devices wegputzen — ein Restore ERSETZT den Bestand,
    // sonst wuerden alte Slices beim naechsten Boot wiederauferstehen.
    bool all = true;
    for (auto& s : speakers_) {
        all = saveSpeakerToNVS_(s.deviceId) && all;
    }
    std::vector<String> ids;
    listPerSpeakerIds_(ids);
    for (auto& id : ids) {
        if (!find_(id)) eraseStoreKeys_(id.c_str());
    }
    // Liegt (nach unvollstaendiger Migration) noch ein Legacy-Blob herum,
    // ist er nach einem VOLLSTAENDIG persistierten Restore obsolet.
    if (all) {
        eraseStoreKeys_(LEGACY_KEY);
        legacyLoadFailed_ = false;
        loadFailed_ = !loadFailedIds_.empty();
    }
    return all;
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
    return saveSpeakerToNVS_(deviceId);
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
    if (changed) return saveSpeakerToNVS_(deviceId);
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
    // Wird der Slice dadurch komplett leer, LOESCHT saveSpeakerToNVS_ den
    // Key — d.h. Preset-Loeschen kann jetzt auch an der vollen Partition
    // Platz freigeben statt am eigenen Write zu scheitern.
    saveSpeakerToNVS_(deviceId);
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
        // Pro Ziel ein kleiner Slice-Write (statt frueher 1 Gesamt-Write):
        // ein volles Ziel blockiert die anderen nicht mehr; Fehlschlaege
        // zaehlt saveSpeakerToNVS_ zentral.
        saveSpeakerToNVS_(tgtId);
        ++n;
    }
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

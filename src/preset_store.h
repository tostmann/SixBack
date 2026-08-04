// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Preset-Store
//
// Verwaltet pro Speaker eine Liste von max. 6 Presets, persistent in NVS.
// Plus Gruppen-Mechanik: ein Speaker kann Mitglied einer Gruppe sein,
// und ein PUT auf /api/groups/{groupId}/preset/{n} propagiert das Preset
// in alle Gruppen-Mitglieder.
//
// Preset kann sein:
//   - TuneIn-Station (sourceType=TUNEIN, stationId="s24896")
//   - Direkter Stream-URL (sourceType=LOCAL_INTERNET_RADIO, url="http://...")
//   - Spotify, Pandora etc. spaeter
#ifndef BOSEFIX32_PRESET_STORE_H
#define BOSEFIX32_PRESET_STORE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sixback {

enum class PresetSource : uint8_t {
    EMPTY                = 0,
    TUNEIN               = 1,
    LOCAL_INTERNET_RADIO = 2,
    // OPAQUE = Preset einer Source-Klasse die SixBack nicht selbst
    // aufloest (z.B. STORED_MUSIC, UPNP, BLUETOOTH-Preset). Wir speichern
    // das komplette urspruengliche <ContentItem>-XML in rawContentItem und
    // reichen es beim Sync 1:1 zurueck — der Speaker kommuniziert direkt
    // mit dem DLNA-Server / Bluetooth-Stack, die Cloud (= unser ESP) ist
    // am Playback nicht beteiligt.
    OPAQUE               = 3,
};

struct Preset {
    uint8_t      slot;          // 1..6
    PresetSource source;
    String       name;          // z.B. "SWR3"
    String       stationId;     // bei TUNEIN: "s24896"
    String       streamUrl;     // bei LOCAL_INTERNET_RADIO: "http://liveradio.swr.de/tn2d2ac/swr3"
    String       imageUrl;      // optional
    // Bei source==OPAQUE: das vollstaendige <ContentItem>...</ContentItem>
    // XML wie wir es vom Speaker gesehen haben. Wird 1:1 ins toBoseXml
    // eingebettet. Bei allen anderen Source-Typen leer.
    String       rawContentItem;
    // Original-Source-Bezeichnung bei OPAQUE (z.B. "STORED_MUSIC_MEDIA_RENDERER")
    // — fuer UI-Anzeige und Debugging. Bei native Sources (TUNEIN, ...)
    // leer.
    String       opaqueSourceName;
};

class PresetStore {
public:
    static PresetStore& instance();

    // RAII-Lock fuer Caller die ueber mehrere Aufrufe konsistente Sicht
    // brauchen. Rekursiv. Innen-Methoden locken sich ohnehin selbst.
    class LockGuard {
    public:
        explicit LockGuard(PresetStore& ps);
        ~LockGuard();
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        PresetStore& ps_;
    };

    void loadFromNVS();
    // Vollsweep: schreibt JEDEN Slice einzeln (leere werden geloescht) und
    // raeumt Keys verwaister Devices weg. Nur noch fuer Import/Restore —
    // Einzel-Aenderungen laufen intern ueber saveSpeakerToNVS_ (1 Slice).
    // true = alle Slices committed; false = mindestens einer scheiterte.
    bool saveToNVS();

    // Lese alle 6 Slots fuer einen Speaker; leere Slots haben source=EMPTY.
    std::vector<Preset> getForSpeaker(const String& deviceId);
    // Lese Preset fuer einen Slot (1..6).
    Preset get(const String& deviceId, uint8_t slot);
    // Setze ein Preset (slot 1..6). Persistiert (1 NVS-Write).
    bool   set(const String& deviceId, const Preset& p);
    // Setze N Presets fuer einen Speaker in einem Schwung — 1 NVS-Write
    // statt N. Wird vom importFromDevice-Pfad genutzt, sonst sind das
    // 6 Slots × ein write pro Speaker.
    bool   setSlots(const String& deviceId, const std::vector<Preset>& presets);
    // Leeren Slot setzen (delete).
    bool   clear(const String& deviceId, uint8_t slot);

    // Gruppen-Operationen: kopiert die Presets eines Source-Speakers auf alle
    // anderen Speaker mit gleicher groupId (oder explicit deviceId-Liste).
    int syncToGroup(const String& sourceDeviceId,
                    const std::vector<String>& targetDeviceIds);

    // Komplett-JSON fuer Backup/Restore.
    void exportJson(JsonDocument& out);
    bool importJson(JsonDocument& in);

    // Fuer den Speaker-Cloud-Mock (/bmx/marge ...): liefert die Preset-Liste
    // im Bose-XML-Format - alle Slots fuer den deviceId.
    String toBoseXml(const String& deviceId);

    // True wenn fuer das Device mindestens ein nicht-EMPTY-Slot existiert.
    // Genutzt von den Cloud-Mock-Endpoints um zu entscheiden, ob ueberhaupt
    // ein <presets>-Element ausgegeben wird — bei "false" liefern wir 404
    // (Cloud sagt "kein Mandat") und der Speaker behaelt seinen lokalen
    // Preset-Cache statt ihn mit unserer leeren Liste zu ueberschreiben.
    // Das ist die wichtigste Schutzlinie gegen Preset-Verlust durch Migration
    // wenn der Store nach Erase noch leer ist.
    bool hasAnyFor(const String& deviceId);

    // Hot-Path-Lookup fuer TuneIn-Resolve: iteriert alle Speaker × 6 Slots
    // und liefert das erste Preset mit stationId==id zurueck. Vermeidet
    // exportJson()-Builds, die bei jedem Preset-Druck am Speaker getriggert
    // werden.  Liefert true wenn gefunden + befuellt out.
    bool findByStationId(const String& stationId, Preset& out);

    // --- Forensik (FHEM 144729 #153, 2026-07-17) -------------------------
    // true = beim Boot LAG ein Store-Blob in NVS, war aber UNLESBAR
    // (hs-decode-/getBytes-/JSON-Parse-Fehler) -> Store startete leer,
    // OBWOHL der User Presets hatte. handleAccountFull gated darauf mit
    // 404 (Speaker behalten ihren Cache), bis der erste erfolgreiche
    // saveToNVS den defekten Blob ersetzt hat. Fresh-Install (kein Blob)
    // setzt das Flag NICHT.
    bool     loadFailedFromNvs() const { return loadFailed_; }
    // Fehlgeschlagene saveToNVS() seit Boot (inkl. JsonDocument-Overflow),
    // zentral gezaehlt fuer ALLE Caller (set/setSlots/clear/syncToGroup/
    // importJson) — runtime-only, via /api/status preset_store.save_fails.
    uint32_t saveFails() const         { return saveFails_; }
    // Aufschluesselung nach URSACHE (v0.8.38, FHEM 144729 msg1367274). Der
    // Summenzaehler oben mischt zwei GEGENSAETZLICHE Faelle, was einen
    // Feld-Dump nicht diagnostizierbar macht:
    //   saveHeapAborts: JsonDocument lief bei Heap-Knappheit ueber, der Save
    //     wurde BEWUSST abgebrochen. NVS bleibt intakt, RAM vollstaendig, der
    //     naechste Save heilt -> transient, KEIN Datenverlust.
    //   saveNvsFails:   nvsSaveJsonWithCleanup() hat abgelehnt = echter
    //     Schreibfehler (Platz/Partition) -> DAS ist die Kapazitaetskante.
    // Gleiche Logik wie die Reject-Aufschluesselung im OutboundGuard
    // (rej_floor_total/rej_floor_block/rej_inflight, FHEM 144729 #155):
    // ohne sie ist "save_fails: 1" im Feld nicht bewertbar.
    uint32_t saveHeapAborts() const    { return saveHeapAborts_; }
    uint32_t saveNvsFails() const      { return saveNvsFails_; }
    // Monoton steigend bei JEDEM erfolgreichen saveToNVS. Dient als
    // "hat sich an der NVS-Lage etwas gebessert?"-Generation: der
    // Auto-Import-Backoff (api_endpoints.cpp) verwirft seine Sperrliste,
    // sobald irgendein Save wieder durchkam — sonst bliebe ein einmal
    // gescheiterter Import bis zum Reboot gesperrt, auch nachdem der
    // User Platz geschaffen hat.
    uint32_t saveOkCount() const       { return saveOkCount_; }
    // Anzahl Speaker-Eintraege im Store (Boot-Forensik: wie viele Slices
    // der Load tatsaechlich geliefert hat).
    size_t   speakerCount();

private:
    PresetStore() = default;

    // Pro Speaker: 6 Slots im Vector (slot=1 ist [0], slot=6 ist [5]).
    // Map-aehnlich: deviceId -> array.
    struct PerSpeaker {
        String  deviceId;
        Preset  slots[6];
    };
    std::vector<PerSpeaker> speakers_;

    PerSpeaker* findOrCreate_(const String& deviceId);
    PerSpeaker* find_(const String& deviceId);
    void initMutex_();

    // --- Per-Speaker-Persistenz (v0.8.40, FHEM 144729 NVS-Kante) -----------
    // Jeder Speaker liegt als EIGENER NVS-Key (Key = deviceId, 12-hex-MAC;
    // A/B-Geschwister "<id>~"/"<id>#0"/"<id>#1" macht nvs_helper). Ein Save
    // schreibt nur noch den betroffenen Slice (~0,5-1 KB) statt des
    // Gesamtbestands — der EINE grosse Write war die Kapazitaetskante, an
    // der ein 9-Boxen-Store jede Aenderung blockierte (msg1367457:
    // NOT_ENOUGH_SPACE bei blob=4846). Der Legacy-Gesamtblob ("presets")
    // wird beim ersten Boot migriert und dann geloescht.
    void parseSlice_(JsonObject ps, PerSpeaker& s);            // NVS-JSON -> Slice
    void buildSliceDoc_(const PerSpeaker& s, JsonDocument& out); // Slice -> NVS-JSON
    bool saveSpeakerToNVS_(const String& deviceId);   // Caller haelt den Lock
    void migrateLegacyBlob_(const std::vector<String>& haveSlice);
    void healLoadFail_(const String& deviceId);       // Slice-Save heilt Load-Fail

    mutable SemaphoreHandle_t mx_ = nullptr;
    bool     loadFailed_ = false;   // abgeleitet: legacyLoadFailed_ || Slices
    bool     legacyLoadFailed_ = false;   // Legacy-Gesamtblob vorhanden, unlesbar
    std::vector<String> loadFailedIds_;   // Per-Speaker-Slices vorhanden, unlesbar
    uint32_t saveFails_  = 0;       // saveToNVS-Fehlschlaege seit Boot (Summe)
    uint32_t saveHeapAborts_ = 0;   // davon: JsonDocument-Overflow (transient)
    uint32_t saveNvsFails_   = 0;   // davon: echter NVS-Schreibfehler
    uint32_t saveOkCount_    = 0;   // erfolgreiche saveToNVS seit Boot
};

const char* presetSourceToStr(PresetSource s);
PresetSource presetSourceFromStr(const String& s);

// Baut die ContentItem-location fuer einen Custom-Stream-Preset
// (LOCAL_INTERNET_RADIO) ueber den nativen Bose-ORION-Adapter:
//   "/station?data=<urlsafe-base64-json>"  mit JSON
//   {streamUrl,name,imageUrl,streamType:"liveRadio",isRealtime:true}.
// Der Speaker loest das baseUrl-relativ ueber svc-bmx-adapter-orion auf
// (-> handleOrionStation, bmx_services.json LIR/provider 11). Roh-Stream-URL
// als location spielt der Speaker NICHT (siehe
// reference_bose_local_internet_radio_speaker_save_broken); der ORION-Pfad
// schon (empirisch v0.8.11, Emma). Deterministisch -> push/diff bleiben stabil.
String orionStationLocation(const String& streamUrl, const String& name,
                            const String& imageUrl);

// XML-escapt Text fuer ContentItem-Attribute / <itemName> (& < > " '). Public,
// damit der direkte /select-Push (api_endpoints.cpp) denselben Escaper nutzt
// wie toBoseXml — sonst verstuemmelt ein '&' im Namen das ContentItem-XML und
// der Speaker droppt es ("Radio Bella & Monella" -> "Radio Bella  Monella").
String escapeXml(const String& in);

// Inverse von escapeXml: dekodiert XML-Entities (&amp; &lt; &gt; &quot; &apos;)
// im aus Speaker-XML extrahierten Text-Content zurueck zur logischen Form.
// Pflicht beim PARSEN — sonst landet "&amp;" in Store/UI und der Diff vergleicht
// den logischen Store-Namen ("&") gegen den HW-Namen ("&amp;") -> Mismatch ->
// Endlos-Re-Push. Symmetrisch zu escapeXml/toBoseXml.
String unescapeXml(const String& in);

} // namespace sixback

#endif

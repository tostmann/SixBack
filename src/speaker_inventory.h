// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Speaker-Inventory
//
// Verwaltet die Liste aller im LAN erkannten Bose-SoundTouch-Speaker:
//   - SSDP-Discovery (Multicast M-SEARCH)
//   - Active-Scan im aktuellen Subnetz als Fallback
//   - GET /info am Speaker-Port 8090 fuer Metadata
//   - Telnet /getpdo CurrentSystemConfiguration fuer Migrations-Status
//   - Persistenz in NVS
//   - Gruppen-Zugehoerigkeit fuer Preset-Sync
#ifndef BOSEFIX32_SPEAKER_INVENTORY_H
#define BOSEFIX32_SPEAKER_INVENTORY_H

#include <Arduino.h>
#include <atomic>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sixback {

enum class MigrationStatus : uint8_t {
    UNKNOWN         = 0,  // nie geprueft / Telnet-Antwort nicht parsebar
    NOT_MIGRATED    = 1,  // /getpdo zeigt streaming.bose.com etc. (Originalzustand)
    MIGRATED        = 2,  // /getpdo zeigt eine lokale Replacement-URL (irgendwo - kann veraltet sein)
    OFFLINE         = 3,  // Speaker komplett unreachable (weder Telnet noch BMX)
    SETTLING        = 4,  // Telnet down, aber BMX-API (Port 8090) up — Speaker
                          //   lebt, Diag-Shell transient nicht da (typisch waehrend
                          //   Cloud-Migration-Reboot). Wird beim naechsten Refresh
                          //   neu klassifiziert.
};

struct Speaker {
    String deviceId;        // "EC24B8D4910D" (aus /info deviceID)
    String name;            // "Bose SoundTouch Kueche"
    String model;           // "SoundTouch 30"
    String firmware;        // "27.0.6.46330.5043500..."
    String moduleType;      // /info <moduleType> (sm2/scm = Wireless-Modul-Generation)
    String variant;         // /info <variant> (rhino/mojo/spotty) — HW-Revision fuer Issue-Triage
    String ip;              // "192.168.1.50"
    String accountId;       // margeAccountUUID
    MigrationStatus status; // siehe oben
    String cloudUrl;        // aktuelle margeServerUrl am Speaker (fuer UI-Anzeige)
    bool ownedByUs = false; // TRUE = wir haben den Speaker je via /api/.../migrate
                            // konfiguriert -> ip_failsafe re-migriert ihn bei
                            // jedem ESP-IP-Wechsel. FALSE = noch nie hier,
                            // oder vom User reverted.
    bool sourcesReady = true; // Nur fuer zu-UNS-migrierte Speaker aussagekraeftig:
                            // TRUE = der SixBack-account/full-Source-Block ist am
                            // Speaker registriert (TUNEIN als READY in /sources).
                            // FALSE = migriert, aber Sources nie angewendet
                            // (Issue #10: push -> /select=500). Wird bei jedem
                            // refreshMigrationStatus neu ermittelt; UI warnt +
                            // bietet Re-Sync. Runtime-derived, nicht in NVS.
    uint8_t offlineStreak = 0; // Runtime-only (NICHT NVS): Anzahl aufeinanderfolgender
                            // Refresh-Zyklen mit nicht-gesunder Probe (OFFLINE/SETTLING/
                            // UNKNOWN). Entprellt den UI-Status-Flip — die Kachel kippt
                            // erst nach STATUS_OFFLINE_DEBOUNCE Schlecht-Zyklen, nicht
                            // nach einem einzelnen verfehlten Durchlauf (FHEM #49 fred).
    uint32_t lastSeenMs;
    String groupId;         // freitext, default ""
    bool hidden = false;    // WebUI-Ausblendung (fremde/Nachbar-Boxen, FHEM
                            // 144729). Reine Anzeige-Semantik: Auto-Mode/
                            // Cloud-Mock/BMX bedienen den Speaker weiter.
                            // Persistiert im Inventory-Blob; mergeSpeaker_
                            // fasst es nicht an -> re-discovery-fest.

    // DLNA-Server-UUIDs die dieser Speaker via /listMediaServers sieht.
    // Werden vom Speaker bei jedem account/full erwartet als sourceAccount-
    // Match damit STORED_MUSIC-Presets nicht als "Quelle nicht vorhanden"
    // verworfen werden. Liste wird bei Migrate/Refresh aktualisiert.
    std::vector<String> mediaServerUuids;

    // Spotify-Accounts die der Speaker via BMX /sources als READY meldet.
    // Pre-Bose-Cloud-Shutdown verknuepft, eSDK-Token im Speaker-NVS. Aus
    // dem <sourceItem source="SPOTIFY" sourceAccount="..." status="READY">
    // ausgelesen. Diagnose-Only (Stufe 0) — Migration-Persistenz folgt mit
    // OAuth-Flow + account/full-Token-Injection (Stufe 1).
    struct SpotifyAccount {
        String sourceAccount;   // z.B. "fredfeuerstein1972" oder opaque Spotify-User-ID
        String displayName;     // z.B. "fred@herr-der-mails.de" (XML-Text-Inhalt)
    };
    std::vector<SpotifyAccount> spotifyAccounts;
};

enum class ProbeFailReason : uint8_t {
    OK = 0,
    HTTP_BEGIN,        // http.begin() returnte false (URL kaputt)
    CONNECT_FAILED,    // negative HTTPClient-Code (Timeout, Connection refused)
    HTTP_NOT_200,      // Speaker antwortet, aber nicht mit 200
    EMPTY_BODY,        // HTTP 200, aber Body 0 Bytes
    WRONG_BODY,        // HTTP 200, aber <info ...> nicht gefunden
    NO_DEVICE_ID,      // <info> da, aber deviceID-Attribut leer/fehlt
};

struct ProbeFailure {
    ProbeFailReason reason = ProbeFailReason::OK;
    String          detail;
};

const char* probeFailReasonStr(ProbeFailReason r);

class SpeakerInventory {
public:
    static SpeakerInventory& instance();

    // RAII-Lock fuer externe Halter eines Speaker*-Pointers. Rekursiv —
    // public-Methoden duerfen den Lock auch nesten. Pflicht fuer jeden
    // Caller, der findById()/findByIp() ueber mehrere Operationen hinweg
    // benutzt; sonst kann der zugrundeliegende vector reallocaten und der
    // Pointer dangelt.
    class LockGuard {
    public:
        explicit LockGuard(SpeakerInventory& inv);
        ~LockGuard();
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        SpeakerInventory& inv_;
    };

    // Laedt persistierte Speaker aus NVS in den RAM-cache. Liest die
    // Per-Speaker-Slices (>= v0.8.43); liegt (auch zusaetzlich, nach einer
    // unterbrochenen Migration) noch der Legacy-Gesamtblob "speakers"
    // (<= v0.8.42), werden dessen noch nicht geslicete Speaker ergaenzt
    // und die Migration erneut angestossen.
    void loadFromNVS();

    // Persistiert aktuellen Cache — seit dem Slicing pro Speaker einzeln
    // (eigener NVS-Key je deviceId) plus einem kleinen Order-Key fuer die
    // Anzeige-Reihenfolge. Der A/B-Unchanged-Skip macht den Vollsweep
    // billig: real geschrieben werden nur geaenderte Slices. false = mind.
    // ein Slice-Write fehlgeschlagen (zaehlt saveFails_ hoch — /api/status
    // inventory.save_fails, UI-Badge); die uebrigen Slices sind trotzdem
    // persistiert (kein Alles-oder-nichts mehr wie beim Gesamtblob).
    bool saveToNVS();

    // Fehlgeschlagene saveToNVS() seit Boot (runtime-only, kein NVS).
    uint32_t saveFails() const { return saveFails_; }

    // Zweistufige Discovery: synchron werden knownIpProbe + SSDP-M-SEARCH
    // ausgefuehrt (~5 s), danach return. Der teure /24-Active-Scan laeuft
    // in einer FreeRTOS-Hintergrund-Task — Status via isScanRunning().
    // Sinn: HTTP-Handler kann sofort eine response mit "was wir bisher
    // wissen" liefern; das UI pollt anschliessend /api/speakers bis
    // isScanRunning() wieder false ist.
    //
    // Der /24-Active-Scan laeuft NUR, wenn er etwas finden kann
    // (activeScanWorthwhile_: leeres Inventory oder ein OFFLINE/UNKNOWN-
    // Speaker) ODER forceActiveScan=true (expliziter User-Discover via
    // POST /api/speakers/discover). Sind alle bekannten Speaker erreichbar +
    // klassifiziert, wird der Scan uebersprungen — spart ~66 s + Heap-Druck
    // (auf no-PSRAM-C5 sonst heap_reboot beim Boot-Auto-Mode-Pass).
    void discover(bool forceActiveScan = false);

    // Lightweight-Discovery fuer periodische Cron-Checks: NUR knownIpProbe
    // + SSDP-Burst + refreshMigrationStatus (~6 s total). KEIN /24-Active-
    // Scan im Hintergrund. Geeignet fuer alle paar Minuten ausgefuehrte
    // Auto-Mode-Cron-Ticks, ohne den HTTP-Server zu blockieren oder das
    // LAN zu hammern.
    void discoverSync();

    // True solange der Background-Active-Scan noch laeuft.
    bool isScanRunning() const { return scanRunning_; }

    // Pruft NUR die Migrations-Status (Telnet:17000) - viel schneller als
    // discover() weil keine IP-Scan-Phase.
    void refreshMigrationStatus();

    // Wie discover(): startet refreshMigrationStatus() + Spotify-/sources-Pull
    // je Speaker in einer FreeRTOS-Hintergrund-Task statt synchron im AsyncTCP-
    // Handler. Grund (FHEM 144729 #86): refreshMigrationStatus blockiert je
    // Speaker per Telnet/BMX/Peer-Probe und enthaelt einen stale-view-Retry mit
    // delay() bis 30 s — synchron im Request-Handler fror das die ganze WebUI
    // bei grossen Zonen ein. scanRunning_ wird gesetzt; das UI pollt
    // /api/speakers bis isScanRunning()==false. Spawnt nicht, wenn bereits ein
    // Scan/Refresh laeuft (compare_exchange auf scanRunning_).
    void refreshStatusesAsync();

    // Liefert read-only-Kopie der Liste fuer UI/API. Lock intern.
    std::vector<Speaker> list();

    // Manuelles Hinzufuegen per IP (z.B. wenn SSDP nichts liefert). Wenn
    // `failOut` gesetzt ist und der probe fehlschlaegt, wird der Grund dort
    // abgelegt — fuer Diagnose-API/UI-Anzeige.
    bool addByIp(const String& ip, ProbeFailure* failOut = nullptr);

    // /listMediaServers vom Speaker pullen und die UUIDs in seinem Speaker-
    // Eintrag persistieren. Wird vom Migrate-Flow aufgerufen damit
    // STORED_MUSIC-Presets nach Cloud-Wechsel weiter funktionieren (siehe
    // Memo reference-bosefix32-stored-music-source-decl).
    void refreshMediaServers(const String& deviceId);

    // BMX /sources vom Speaker pullen und alle SPOTIFY READY-Items in
    // Speaker::spotifyAccounts persistieren. Diagnose-Only (Stufe 0): macht
    // die linked Spotify-Accounts pro Speaker im UI sichtbar. Stufe 1 wird
    // OAuth-Flow + Refresh-Token-Injection in account/full ergaenzen damit
    // Spotify nach Migration nicht nach 60min ausfaellt.
    void refreshSpotifyAccounts(const String& deviceId);

    // Loescht einen Speaker aus dem Cache (nicht vom Geraet).
    bool remove(const String& deviceId);

    // Setzt/loescht das WebUI-Ausblendungs-Flag (persistiert). false wenn
    // deviceId unbekannt.
    bool setHidden(const String& deviceId, bool hidden);

    // Setzt die Anzeige-Reihenfolge der Speaker. `deviceIdOrder` listet die
    // gewuenschte Reihenfolge; nicht genannte (neu entdeckte) Speaker bleiben
    // in ihrer bisherigen relativen Reihenfolge hinten angehaengt. Die Order
    // IST die Vektor-Reihenfolge — saveToNVS persistiert sie, daher haelt die
    // Sortierung reboot- UND browseruebergreifend (kein localStorage). Liefert
    // immer true (unbekannte IDs werden ignoriert, nicht als Fehler gewertet).
    bool reorder(const std::vector<String>& deviceIdOrder);

    // Liefert Speaker per deviceId oder ip (nullable).
    Speaker* findById(const String& deviceId);
    Speaker* findByIp(const String& ip);

    // Migration-Status fuer Single-Speaker (Telnet-Call).
    // Liefert Status + (Ausgabe-Parameter) das aktuell konfigurierte
    // margeServerUrl-Target, damit das UI zeigen kann WO der Speaker
    // gerade hin migriert ist (z.B. Pi5 statt unser ESP).
    MigrationStatus detectStatus(const String& ip, const String& myBaseUrl,
                                  String* outCloudUrl = nullptr);

private:
    SpeakerInventory() = default;
    uint32_t saveFails_ = 0;   // fehlgeschlagene saveToNVS() seit Boot
    // Eine Speaker-Slice unter ihrem deviceId-Key persistieren (A/B-Save
    // via nvsSaveJsonWithCleanup). Zaehlt NICHT selbst — saveFails_ zaehlt
    // zentral in saveToNVS().
    bool saveSpeakerToNVS_(const Speaker& s);
    // Anzeige-Reihenfolge ({"ids":[...]} unter "order") persistieren;
    // bei leerem Inventory wird der Key geloescht statt leer geschrieben.
    bool saveOrderToNVS_();
    // Legacy-Gesamtblob -> Slices. Phase A schreibt NEBEN den Blob; passt
    // das nicht (Partition eng), Druck-Migration: Blob zuerst loeschen
    // (Stand liegt vollstaendig im RAM), dann erneut. Verbleibende Fails
    // lassen die Slice RAM-only bis Platz frei wird.
    void migrateLegacyBlob_();
    // Migrations-Guard: rechnet den Entry-Bedarf aller noch fehlenden
    // Slices (haveSliceIds = schon vorhandene, deren Re-Save der
    // Unchanged-Skip gratis macht) gegen verfuegbare Entries + das, was
    // das Loeschen des Legacy-Blobs freigibt. Bewusst KONSERVATIV (obere
    // Schranke via Klartext-Laenge + Gate-Marge je Slice): ein falsches
    // "GO" strandet die Druck-Migration nach geloeschtem Blob — ein
    // falsches "STAY-LEGACY" kostet nur einen Boot Aufschub.
    bool migrationFits_(const std::vector<String>& haveSliceIds);
    // Fallback-Persistenz fuer Partitionen, auf denen migrationFits_ die
    // Slice-Migration ablehnt: der Gesamtblob-Save wie vor dem Slicing.
    bool saveLegacyBlobToNVS_();
    // Sortiert speakers_ nach der Id-Liste; nicht genannte bleiben in
    // bisheriger relativer Reihenfolge hinten. Gemeinsamer Kern von
    // reorder() (mit Save) und loadFromNVS() (ohne).
    void applyOrder_(const std::vector<String>& deviceIdOrder);
    void mergeSpeaker_(const Speaker& s);
    bool probeIp_(const String& ip, Speaker& out,
                  uint16_t connectMs = 800, uint16_t readMs = 1500,
                  ProbeFailure* failOut = nullptr);
    void knownIpProbe_();
    void ssdpMSearch_();
    // True, wenn der /24-Active-Scan etwas finden kann (siehe discover()):
    // leeres Inventory oder mind. ein OFFLINE/UNKNOWN-Speaker. Nach der
    // Sync-Phase auszuwerten.
    bool activeScanWorthwhile_();
    void activeScan_();
    static void activeScanTask_(void* arg);  // FreeRTOS entry
    static void refreshStatusTask_(void* arg);  // FreeRTOS entry (manueller Refresh)

    void initMutex_();

    std::atomic<bool> scanRunning_{false};

    // true = per-Speaker-Slices (Normalfall, auch fresh), false = die
    // Partition war beim Boot zu voll fuer eine sichere Slice-Migration
    // (migrationFits_) -> Persistenz laeuft weiter ueber den Legacy-
    // Gesamtblob. Wird nur in loadFromNVS() entschieden; der naechste
    // Boot prueft neu.
    bool slicedMode_ = true;

    mutable SemaphoreHandle_t mx_ = nullptr;

    std::vector<Speaker> speakers_;
};

const char* migrationStatusToStr(MigrationStatus s);

// Probt ob unter `url` ein LEBENDER SixBack-Stick antwortet (GET / mit
// kurzem Timeout; die Cloud-Mock-Startseite traegt den Marker "SixBack").
// Basis der peer-aware-Skip-Logik in auto_mode UND der Disown-Semantik in
// refreshMigrationStatus: nur ein VERIFIZIERTER fremder Owner (lebender
// Peer / Bose-Revert) rechtfertigt ein Auto-Release — eine tote URL ist
// ambig (eigene alte IP nach DHCP-Wechsel oder entsorgter Zweit-Stick).
bool isPeerSixBackCloud(const String& url);

} // namespace sixback

#endif

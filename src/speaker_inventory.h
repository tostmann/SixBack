// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Speaker-Inventory
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

namespace bosefix {

enum class MigrationStatus : uint8_t {
    UNKNOWN         = 0,  // nie geprueft / Telnet-Fehler
    NOT_MIGRATED    = 1,  // /getpdo zeigt streaming.bose.com etc. (Originalzustand)
    MIGRATED        = 2,  // /getpdo zeigt eine lokale Replacement-URL (irgendwo - kann veraltet sein)
    OFFLINE         = 3,  // Telnet TCP-Connect fehlgeschlagen
};

struct Speaker {
    String deviceId;        // "EC24B8D4910D" (aus /info deviceID)
    String name;            // "Bose SoundTouch Kueche"
    String model;           // "SoundTouch 30"
    String firmware;        // "27.0.6.46330.5043500..."
    String ip;              // "10.10.11.196"
    String accountId;       // margeAccountUUID
    MigrationStatus status; // siehe oben
    String cloudUrl;        // aktuelle margeServerUrl am Speaker (fuer UI-Anzeige)
    bool ownedByUs = false; // TRUE = wir haben den Speaker je via /api/.../migrate
                            // konfiguriert -> ip_failsafe re-migriert ihn bei
                            // jedem ESP-IP-Wechsel. FALSE = noch nie hier,
                            // oder vom User reverted.
    uint32_t lastSeenMs;
    String groupId;         // freitext, default ""
};

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

    // Laedt persistierte Speaker aus NVS in den RAM-cache.
    void loadFromNVS();

    // Persistiert aktuellen Cache.
    void saveToNVS();

    // Zweistufige Discovery: synchron werden knownIpProbe + SSDP-M-SEARCH
    // ausgefuehrt (~5 s), danach return. Der teure /24-Active-Scan laeuft
    // in einer FreeRTOS-Hintergrund-Task — Status via isScanRunning().
    // Sinn: HTTP-Handler kann sofort eine response mit "was wir bisher
    // wissen" liefern; das UI pollt anschliessend /api/speakers bis
    // isScanRunning() wieder false ist.
    void discover();

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

    // Liefert read-only-Kopie der Liste fuer UI/API. Lock intern.
    std::vector<Speaker> list();

    // Manuelles Hinzufuegen per IP (z.B. wenn SSDP nichts liefert).
    bool addByIp(const String& ip);

    // Loescht einen Speaker aus dem Cache (nicht vom Geraet).
    bool remove(const String& deviceId);

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
    void mergeSpeaker_(const Speaker& s);
    bool probeIp_(const String& ip, Speaker& out,
                  uint16_t connectMs = 800, uint16_t readMs = 1500);
    void knownIpProbe_();
    void ssdpMSearch_();
    void activeScan_();
    static void activeScanTask_(void* arg);  // FreeRTOS entry

    void initMutex_();

    std::atomic<bool> scanRunning_{false};

    mutable SemaphoreHandle_t mx_ = nullptr;

    std::vector<Speaker> speakers_;
};

const char* migrationStatusToStr(MigrationStatus s);

} // namespace bosefix

#endif

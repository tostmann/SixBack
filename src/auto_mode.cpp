// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — Auto-Mode (Zero-Touch Migration + Preset-Restore)

#include "auto_mode.h"
#include "config.h"
#include "nvs_helper.h"
#include "preset_store.h"
#include "source_normalizer.h"
#include "speaker_inventory.h"
#include "speaker_telnet.h"
#include "speaker_diagnostic.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace sixback {

namespace {

constexpr const char* NVS_NS  = "sixback-auto";
constexpr const char* NVS_KEY = "config";

AutoModeStatus    g_status;
SemaphoreHandle_t g_statusMx   = nullptr;
volatile bool     g_taskSpawned = false;

void initMutex_() {
    if (!g_statusMx) g_statusMx = xSemaphoreCreateMutex();
}

// Mutex-Guard im Mini-Format. Lock/unlock im Block-Scope.
struct Lock_ {
    Lock_()  { if (g_statusMx) xSemaphoreTake(g_statusMx, portMAX_DELAY); }
    ~Lock_() { if (g_statusMx) xSemaphoreGive(g_statusMx); }
};

void setState_(const String& s) {
    {
        Lock_ l;
        g_status.state = s;
    }
    Serial.printf("[auto] state=%s\n", s.c_str());
}

void setError_(const String& s) {
    {
        Lock_ l;
        g_status.lastError = s;
    }
    Serial.printf("[auto] ERROR %s\n", s.c_str());
}

String myBase_() {
    return "http://" + WiFi.localIP().toString() + ":" + String(BOSE_HTTP_PORT);
}

// Mini-XML-Helper (lokal, vermeidet Cross-Datei-Coupling mit api_endpoints).
String xmlAttr_(const String& xml, int beg, int end, const char* attr) {
    String needle = String(attr) + "=\"";
    int s = xml.indexOf(needle, beg);
    if (s < 0 || s > end) return "";
    s += needle.length();
    int e = xml.indexOf('"', s);
    if (e < 0 || e > end) return "";
    return xml.substring(s, e);
}

String xmlTag_(const String& xml, int beg, int end, const char* tag) {
    String open  = String("<")  + tag + ">";
    String close = String("</") + tag + ">";
    int s = xml.indexOf(open, beg);
    if (s < 0 || s > end) return "";
    s += open.length();
    int e = xml.indexOf(close, s);
    if (e < 0 || e > end) return "";
    return xml.substring(s, e);
}

// Holt /presets vom Speaker, normalisiert die Slots, schreibt in PresetStore.
// Liefert (converted, abandoned, totalParsed). Returns false bei HTTP-Fehler.
bool importAndNormalizePresets_(const Speaker& s,
                                int& converted, int& abandoned, int& total) {
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(2000); http.setTimeout(4000);
    String url = "http://" + s.ip + ":" + String(BOSE_BMX_PORT) + "/presets";
    if (!http.begin(url)) return false;
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String xml = http.getString();
    http.end();

    int pos = 0;
    std::vector<Preset> toSet;  // batched-save am Ende statt einer pro Slot
    toSet.reserve(6);
    while (true) {
        int presetOpen = xml.indexOf("<preset id=\"", pos);
        if (presetOpen < 0) break;
        int idStart = presetOpen + 12;
        int idEnd   = xml.indexOf('"', idStart);
        if (idEnd < 0) break;
        int presetClose = xml.indexOf("</preset>", idEnd);
        if (presetClose < 0) break;

        uint8_t slot = xml.substring(idStart, idEnd).toInt();
        if (slot < 1 || slot > 6) { pos = presetClose; continue; }
        ++total;

        String src  = xmlAttr_(xml, idEnd, presetClose, "source");
        String loc  = xmlAttr_(xml, idEnd, presetClose, "location");
        String name = xmlTag_ (xml, idEnd, presetClose, "itemName");
        String img  = xmlTag_ (xml, idEnd, presetClose, "containerArt");

        Preset p;
        p.slot = slot;
        auto nr = normalizePreset(src, loc, name, img, p);
        Serial.printf("[auto]   slot %u source=%s → %s (%s)\n",
                      slot, src.c_str(),
                      normalizeStatusToStr(nr.status), nr.reason.c_str());

        if (nr.status == NormalizeStatus::ABANDONED) {
            ++abandoned;
        } else {
            if (nr.status == NormalizeStatus::OK_CONVERTED) ++converted;
            if (nr.status == NormalizeStatus::OK_OPAQUE) {
                // OPAQUE-Slot: vollstaendiges <ContentItem>...</ContentItem>
                // muss raw mitkopiert werden, sonst kann handleAccountFull
                // den Slot beim Sync nicht rekonstruieren und skipt ihn
                // silent (locStart < 0 → continue). Identisch zu
                // api_endpoints.cpp::importPresetsFromSpeaker_ — diverging
                // Copy hier nachgezogen 2026-05-22 Pre-Release-Test.
                int ciOpen  = xml.indexOf("<ContentItem", idEnd);
                int ciClose = xml.indexOf("</ContentItem>", ciOpen);
                if (ciOpen >= 0 && ciOpen < presetClose && ciClose > ciOpen) {
                    p.rawContentItem = xml.substring(ciOpen, ciClose + 14);
                }
                if (p.name.length() == 0) {
                    p.name = String("[") + src + String("] preset");
                }
            }
            toSet.push_back(p);
        }
        pos = presetClose;
    }
    PresetStore::instance().setSlots(s.deviceId, toSet);  // 1 NVS-Write
    return true;
}

// Wartet bis Speaker nach Reboot wieder /info beantwortet.
bool waitForSpeakerBack_(const String& ip, uint32_t timeoutMs) {
    uint32_t start = millis();
    // erste 30 s: Speaker reboot't gerade — Probes sind aussichtslos.
    // Chunked delay statt monolithische delay(30000): (a) erlaubt WDT-Reset
    // falls die Task spaeter mal esp_task_wdt_add(NULL) bekommt, (b) der UI
    // bekommt ueber g_status.state einen sichtbaren Sekunden-Countdown.
    for (int sec = 30; sec > 0; --sec) {
        {
            Lock_ l;
            g_status.state = String("wait-reboot (") + sec + "s)";
        }
        delay(1000);
    }
    setState_("wait-reboot probing");
    while (millis() - start < timeoutMs) {
        HTTPClient http;
        http.setReuse(false);
        http.setConnectTimeout(1500); http.setTimeout(2000);
        String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/info";
        if (http.begin(url)) {
            int code = http.GET();
            http.end();
            if (code == 200) return true;
        }
        delay(5000);
    }
    return false;
}

// Peer detection: isPeerSixBackCloud() lebt seit dem (b)-Disown-Umbau
// (2026-06-06) in speaker_inventory.{h,cpp} — refreshMigrationStatus
// braucht denselben Probe fuer sein Auto-Release. Semantik unveraendert:
// wenn ein Speaker bereits auf einen anderen LEBENDEN SixBack im LAN
// pollt, sollen wir ihn NICHT zurueckclaimen (verhindert Ping-Pong wenn
// zwei Sticks beide Auto-Mode aktiv haben).
//
// User kann ueber den Manual-Migrate-Endpoint (/api/speaker/<id>/migrate)
// einen Peer-Speaker weiter explizit uebernehmen, das ist ein bewusster
// User-Klick und respektiert die Auto-Mode-Skip-Logik nicht.

bool isEligible_(const Speaker& s, const String& myBase) {
    if (s.ip.length() == 0) return false;
    if (s.cloudUrl == myBase) return false;   // zeigt schon auf uns
    // Defensive: cloudUrl noch leer nach JIT-Resolve im Tick-Pre-Step =
    // Telnet:17000 unreachable. Migration scheitert spaeter an der
    // configure-Phase ohnehin — skip statt try-and-fail (vermeidet
    // "preset import failed"-Spam wenn Speaker am Boot grade settled).
    // Eliminiert die Race wo isPeerSixBackCloud wegen leerem cloudUrl
    // gar nicht gefragt wird → false-positive "eligible".
    if (s.cloudUrl.length() == 0) {
        if (!s.ownedByUs)   // owned+offline: kein Urteil, kein Log-Spam pro Tick
            Serial.printf("[auto] skip %s (%s): cloudUrl unknown (telnet probe failed)\n",
                          s.name.c_str(), s.ip.c_str());
        return false;
    }
    if (s.ownedByUs) {
        // Re-Claim-Pfad ((b)-Semantik 2026-06-06): owned, aber cloudUrl
        // zeigt auf eine fremde Base. refreshMigrationStatus disownt nur
        // bei VERIFIZIERTEM fremden Owner — steht der Speaker hier noch
        // owned, haengt er mutmasslich auf einer Leiche (unsere alte IP
        // nach DHCP-Wechsel, oder ein entsorgter Zweit-Stick). Den holen
        // wir zurueck. KEINE Modell-/FW-Whitelist: der Speaker wurde
        // nachweislich schon einmal erfolgreich migriert — staerkerer
        // Beleg als jede Whitelist (deckt manuell migrierte Modelle ab).
        if (s.cloudUrl.indexOf("bose.com") >= 0) return false;  // Revert = User-Wille
        if (isPeerSixBackCloud(s.cloudUrl)) return false;       // lebender Peer
        Serial.printf("[auto] re-claim %s (%s): owned but parked on dead cloud %s\n",
                      s.name.c_str(), s.ip.c_str(), s.cloudUrl.c_str());
        return true;
    }
    // Modell-Whitelist — auto-mode nur fuer Geraete-Familie, an der wir
    // den Telnet-Pfad empirisch verifiziert haben.
    bool modelOk = s.model.indexOf("SoundTouch 10") >= 0
                || s.model.indexOf("SoundTouch 20") >= 0
                || s.model.indexOf("SoundTouch 30") >= 0;
    if (!modelOk) return false;
    // FW-Whitelist: 27.0.6.x (an Kueche empirisch verifiziert) und
    // 27.0.3.x (an Emma noch NICHT verifiziert — Annahme: gleiche
    // Diag-Shell-Syntax wie 27.0.6, der Speaker stammt aus derselben
    // Generations-Familie sm2/mojo).
    bool fwOk = s.firmware.indexOf("27.0.6.") >= 0
             || s.firmware.indexOf("27.0.3.") >= 0;
    if (!fwOk) return false;
    // Peer-aware (v0.7.5): wenn cloudUrl auf einen anderen SixBack-Stick
    // im LAN zeigt, nicht zurueckclaimen.
    if (s.cloudUrl != myBase && isPeerSixBackCloud(s.cloudUrl)) {
        Serial.printf("[auto] skip %s (%s): peer SixBack at %s already owns it\n",
                      s.name.c_str(), s.ip.c_str(), s.cloudUrl.c_str());
        return false;
    }
    return true;
}

// JIT-Resolve cloudUrl fuer Speaker bei denen die discoverSync-Phase ein
// leeres cloudUrl liefert (Telnet-Probe im Boot-Tick failed: Speaker am
// settling, ARP-Cache kalt, TCP-SYN gedroppt). Aktualisiert sowohl die
// snapshot-Kopie als auch das persistente Inventory + NVS, damit die
// nachfolgenden isEligible_-Aufrufe und das UI den frischen Wert sehen.
//
// Why als separater Step statt JIT-Probe inside isEligible_: isEligible_
// wird im Tick zweimal aufgerufen (Counter + Migrate-Schleife). Wenn der
// JIT dort drin liegt, koennten zwei Telnet-Calls pro empty-cloudUrl-
// Speaker absetzen — Sequenz-Telnet auf Bose-FW ist nicht reentrant-safe,
// die zweite Verbindung wartet ggf. unnoetig.
void resolveEmptyCloudUrls_(std::vector<Speaker>& snapshot, const String& base) {
    int probed = 0, ok = 0;
    for (auto& s : snapshot) {
        if (s.cloudUrl.length() > 0 || s.ip.length() == 0) continue;
        ++probed;
        String resolved;
        MigrationStatus ms = SpeakerInventory::instance()
                                 .detectStatus(s.ip, base, &resolved);
        if (resolved.length() == 0) continue;
        s.cloudUrl = resolved;
        s.status   = ms;
        ++ok;
        SpeakerInventory::LockGuard g(SpeakerInventory::instance());
        if (Speaker* p = SpeakerInventory::instance().findById(s.deviceId)) {
            p->cloudUrl = resolved;
            p->status   = ms;
        }
    }
    if (probed > 0) {
        SpeakerInventory::instance().saveToNVS();
        Serial.printf("[auto] JIT cloudUrl resolve: %d probed, %d resolved\n",
                      probed, ok);
    }
}

// Migration eines Speakers per Wert-Snapshot uebergeben; das vermeidet,
// dass eine Speaker&-Referenz quer durch mehrere Sekunden Telnet/IO
// gehalten wird, waehrend andere Tasks den inventory-vector reallocieren
// koennten. Schreibt das Ergebnis am Schluss unter Lock zurueck.
void migrateOne_(const Speaker& snap) {
    {
        Lock_ l;
        g_status.currentDeviceId = snap.deviceId;
    }
    Serial.printf("[auto] === migrate %s (%s @ %s) ===\n",
                  snap.name.c_str(), snap.deviceId.c_str(), snap.ip.c_str());

    setState_("pre-migrate-snapshot");
    sixback::persistPreMigrateSnapshot(snap.deviceId, /*force=*/false);

    // /listMediaServers vom Speaker pullen, damit handleAccountFull spaeter
    // die uuidToSrcId-Map fuer STORED_MUSIC-Presets aufbauen kann. Ohne diesen
    // Pre-Migrate-Call ist `mediaServerUuids` leer und alle STORED_MUSIC-Slots
    // werden in handleAccountFull silent geskipt (continue auf uuidToSrcId.end())
    // → Speaker bekommt nur die TUNEIN-Slots zurueck und droppt seine
    // DLNA-Presets lokal. Reproduziert 2026-05-22 Pre-Release-Test Iter1-A
    // mit Emma + Kueche (Slot 6 = MiniDLNA "All Music" → ueberlebt nicht).
    // Identisch zur manuellen migrate-Path (api_endpoints.cpp:370).
    SpeakerInventory::instance().refreshMediaServers(snap.deviceId);
    SpeakerInventory::instance().refreshSpotifyAccounts(snap.deviceId);

    setState_("import-presets");
    int converted = 0, abandoned = 0, total = 0;
    if (!importAndNormalizePresets_(snap, converted, abandoned, total)) {
        setError_("preset import failed for " + snap.deviceId);
        return;
    }
    {
        Lock_ l;
        g_status.slotsNormalized += (total - abandoned);
        g_status.slotsConverted  += converted;
        g_status.slotsAbandoned  += abandoned;
    }
    // 2026-05-22: Speaker mit 0 Presets (Erstboot aus der Schachtel,
    // factory-reset, oder Nutzer hat schlicht nie Presets eingerichtet)
    // dennoch migrieren. Sonst bleibt der Speaker auf der toten Bose-Cloud
    // haengen und SixBack tut "nichts". Es gibt nichts zu verlieren — der
    // Preset-Store bleibt fuer das Device leer, spaetere Long-Press-Set's
    // am Knopf werden via Cron-Tick + import-from-device nachgezogen.
    // Beobachtet bei zwei verschiedenen ST20-Snapshots (F45EABFF63E4 /
    // 2C6B7DB9D239) — siehe snapshots ohne presets-Eintrag.
    if (total > 0 && total - abandoned == 0) {
        setError_("speaker " + snap.deviceId + " has presets but all sources unsupported — skip");
        return;
    }
    if (total == 0) {
        Serial.printf("[auto] %s has 0 presets — migrating anyway "
                      "(empty store is fine, future presets push via cron)\n",
                      snap.deviceId.c_str());
    }

    setState_("migrate-telnet");
    auto base = myBase_();
    auto r = migrateSpeaker(snap.ip, base);
    if (!r.ok) {
        setError_("telnet migration failed: " + r.message);
        return;
    }

    setState_("wait-reboot");
    if (!waitForSpeakerBack_(snap.ip, 180000)) {
        setError_("speaker " + snap.ip + " did not come back within 180s");
        return;
    }

    // 2026-05-22: Post-Reboot-Verifikation der margeServerUrl.
    //
    // Reproduzierbar in der Lab-Praxis: migrateSpeaker() liefert r.ok=true
    // und das in-line getpdo direkt nach Telnet-Push zeigt auch oft die
    // neue URL, ABER nach Reboot fallback'd der Speaker manchmal auf eine
    // FRUEHERE margeServerUrl zurueck (vermutlich Diag-Shell-NVS-Race:
    // Push erfolgreich, aber Persistierung in Diag-NVS schlaegt fehl /
    // wird beim Reboot ueberschrieben).  Beobachtet bei Kueche/ST30 in
    // einer migrate-revert-migrate-Schleife: getpdo nach Boot zeigte alte
    // S3-URL obwohl C6-Migrate erfolgreich war → Speaker pollt falschen
    // Cloud-Server, kriegt leeres /full, droppt alle Presets lokal.
    //
    // Fix: nach waitForSpeakerBack_ noch eine zweite getpdo-Runde via
    // captureSysConfigurationList machen, margeServerUrl extrahieren und
    // gegen `base` vergleichen. Bei Mismatch: status auf MIGRATE_FAILED
    // (kein MIGRATED), Inventory bleibt unown'd, naechster Cron-Tick
    // probiert es erneut.
    setState_("verify-postboot");
    String postReply;
    bool gotReply = ::captureSysConfigurationList(snap.ip, postReply);
    if (gotReply) {
        // extrahiere margeServerUrl aus dem getpdo-Reply
        int p = postReply.indexOf("margeServerUrl");
        int t = (p >= 0) ? postReply.indexOf("text:", p) : -1;
        int q1 = (t >= 0) ? postReply.indexOf('"', t) : -1;
        int q2 = (q1 >= 0) ? postReply.indexOf('"', q1 + 1) : -1;
        String actualMarge = (q1 >= 0 && q2 > q1)
            ? postReply.substring(q1 + 1, q2) : String("");

        if (actualMarge.length() > 0 && actualMarge != base) {
            Serial.printf("[auto] VERIFY-FAIL %s: post-reboot margeServerUrl=%s "
                          "expected=%s — NOT marking migrated\n",
                          snap.deviceId.c_str(), actualMarge.c_str(), base.c_str());
            setError_("post-reboot margeServerUrl mismatch: got '" + actualMarge +
                      "', expected '" + base + "'");
            return;
        }
        Serial.printf("[auto] VERIFY-OK %s: post-reboot margeServerUrl=%s\n",
                      snap.deviceId.c_str(), actualMarge.c_str());
    } else {
        // Wenn getpdo nicht mal antwortet: Telnet ist hin, aber BMX
        // (verifiziert durch waitForSpeakerBack_) lebt. Markieren wir trotzdem
        // als MIGRATED, da der Speaker offensichtlich erreichbar ist und die
        // Inline-Verifikation in migrateSpeaker schon einmal OK war. Nur Log.
        Serial.printf("[auto] VERIFY-SKIP %s: post-reboot getpdo no reply — "
                      "marking migrated based on inline check\n",
                      snap.deviceId.c_str());
    }

    auto& inv = SpeakerInventory::instance();
    {
        SpeakerInventory::LockGuard ig(inv);
        if (auto* live = inv.findById(snap.deviceId)) {
            live->status    = MigrationStatus::MIGRATED;
            live->cloudUrl  = base;
            live->ownedByUs = true;
        }
        inv.saveToNVS();
    }

    {
        Lock_ l;
        g_status.speakersMigrated++;
        g_status.currentDeviceId = "";
    }
    Serial.printf("[auto] === %s migrated → %s ===\n",
                  snap.deviceId.c_str(), base.c_str());
}

// Ein kompletter Migrations-Pass. `deep=true` ist die Initial-Boot-Variante
// mit /24-Active-Scan-Wait; `deep=false` ist der Cron-Tick mit Light-Discovery.
// Liefert true, wenn der Pass tatsaechlich gelaufen ist (false z.B. wenn WiFi
// down). Aktualisiert g_status in beiden Faellen.
bool runMigrationPass_(const AutoModeConfig& cfg, bool deep) {
    if (WiFi.status() != WL_CONNECTED) {
        setError_("WiFi not up — skipping pass");
        return false;
    }
    {
        Lock_ l;
        g_status.running   = true;
        g_status.startedMs = millis();
    }
    setState_(deep ? "discovering" : "cron-discovering");
    if (deep) {
        SpeakerInventory::instance().discover();
        uint32_t waitStart = millis();
        while (SpeakerInventory::instance().isScanRunning()
               && millis() - waitStart < 60000) {
            delay(1000);
        }
    } else {
        SpeakerInventory::instance().discoverSync();
    }

    auto snapshot = SpeakerInventory::instance().list();
    String base = myBase_();
    // Fix peer-aware Race (2026-05-26): wenn discoverSync's Telnet-Probe
    // einen Speaker nicht erreicht hat (cloudUrl leer), JIT nachholen.
    // Sonst greift die isPeerSixBackCloud-Skip-Logik nicht und ein frisch
    // gebooteter SixBack wuerde einen Speaker migrieren der schon einem
    // anderen SixBack-Stick im LAN gehoert. Symptom: erster Boot-Tick
    // meldet speakers_eligible=N obwohl alle N auf Peer-Cloud zeigen.
    resolveEmptyCloudUrls_(snapshot, base);
    int eligible = 0;
    for (auto& s : snapshot) if (isEligible_(s, base)) ++eligible;
    {
        Lock_ l;
        g_status.speakersSeen     = (int)snapshot.size();
        g_status.speakersEligible = eligible;
    }
    Serial.printf("[auto] %s done: %d speakers total, %d eligible\n",
                  deep ? "discovery" : "cron-tick",
                  (int)snapshot.size(), eligible);

    if (cfg.dryRun) {
        Serial.println("[auto] dryRun=true — keine echte Migration");
        setState_("done-dryrun");
    } else if (eligible == 0) {
        setState_(deep ? "done-nothing-to-do" : "cron-idle");
    } else {
        uint32_t doneThisPass = 0;
        for (auto& snap : snapshot) {
            // Eligibility wird auf der Snapshot-Kopie gemacht — der State
            // im inventory kann sich zwischen list() und Migration aendern,
            // aber das ist akzeptabel: bei naechstem Cron-Tick neu evaluiert.
            if (!isEligible_(snap, base)) continue;
            if (doneThisPass >= cfg.maxPerBoot) {
                Serial.printf("[auto] maxPerBoot=%u reached — stop\n",
                              cfg.maxPerBoot);
                break;
            }
            migrateOne_(snap);
            ++doneThisPass;
        }
        setState_(deep ? "done" : "cron-idle");
    }

    {
        Lock_ l;
        g_status.ran                = true;
        g_status.running            = false;
        g_status.finishedMs         = millis();
        g_status.lastTickFinishedMs = millis();
        g_status.tickCount++;
        g_status.currentDeviceId    = "";
    }
    return true;
}

void autoModeTask_(void* /*arg*/) {
    AutoModeConfig cfg = loadAutoModeConfig();
    Serial.printf("[auto] startup  enabled=%d  bootDelayMs=%u  dryRun=%d  "
                  "maxPerBoot=%u  cronIntervalS=%u\n",
                  (int)cfg.enabled, cfg.bootDelayMs, (int)cfg.dryRun,
                  cfg.maxPerBoot, cfg.cronIntervalS);

    if (!cfg.enabled) {
        Serial.println("[auto] disabled at boot — task stays alive but idle "
                       "(re-enable + reboot or wait for cron tick reload)");
    } else {
        delay(cfg.bootDelayMs);
        // Config NACH dem Warten neu lesen (FHEM 144729 msg1367658, zweite
        // Haelfte der Zusage). Bis v0.8.43 galt hier die Kopie, die beim
        // Task-Start geladen wurde: das enabled=false-Flag pruefte der Task
        // VOR dem delay, ein waehrend des Fensters umgelegter Schalter lief
        // ins Leere und der Boot-Pass startete trotzdem. Damit war das
        // Fenster nur eine Wartezeit, kein Abschalt-Fenster — ein laengeres
        // haette daran nichts geaendert. Jetzt entscheidet der Stand
        // unmittelbar vor dem ersten Speaker-Zugriff.
        // Bewusst KEIN Abbruch eines bereits laufenden Passes (Laufzeit-Flag
        // zwischen zwei Boxen war erwogen und nicht zugesagt) — der Cron-Loop
        // unten liest ohnehin je Tick neu.
        cfg = loadAutoModeConfig();
        if (!cfg.enabled) {
            Serial.println("[auto] boot pass cancelled — disabled during boot delay");
        } else {
            runMigrationPass_(cfg, /*deep=*/true);
            Serial.printf("[auto] initial pass finished. migrated=%d converted=%d "
                          "abandoned=%d\n",
                          g_status.speakersMigrated, g_status.slotsConverted,
                          g_status.slotsAbandoned);
        }
    }

    // Cron-Loop: alle cronIntervalS Sekunden ein Light-Pass.
    // Reload der Config jedes Tick, damit Toggle via UI ohne Reboot greift.
    while (true) {
        // Config reload + interval (mit Lower-Bound 60 s als Foot-Gun-Guard)
        cfg = loadAutoModeConfig();
        uint32_t intervalMs = (cfg.cronIntervalS < 60 ? 60 : cfg.cronIntervalS) * 1000;
        if (cfg.cronIntervalS == 0) {
            // Cron disabled — schlafe trotzdem, damit Task lebt und Toggle greift
            {
                Lock_ l;
                g_status.sleepStartMs = millis();
                g_status.sleepDurMs   = 60000;
            }
            delay(60000);
            continue;
        }
        // Anker setzen, getAutoModeStatus leitet nextTickInS daraus ab.
        // Kein per-Sekunden-Write mehr unter Lock.
        {
            Lock_ l;
            g_status.sleepStartMs = millis();
            g_status.sleepDurMs   = intervalMs;
        }
        delay(intervalMs);
        // Tick aktivieren
        cfg = loadAutoModeConfig();
        if (!cfg.enabled) {
            Serial.println("[auto] cron tick skipped — disabled");
            continue;
        }
        Serial.printf("[auto] cron tick #%u starting\n", g_status.tickCount + 1);
        runMigrationPass_(cfg, /*deep=*/false);
    }
}

} // anon

AutoModeConfig loadAutoModeConfig() {
    AutoModeConfig c;
    JsonDocument doc;
    if (nvsLoadJson(NVS_NS, NVS_KEY, doc)) {
        // Defensiv: bei vorhandenem NVS-Record aber fehlenden Keys gelten
        // die Image-Defaults aus AutoModeConfig.
        c.enabled       = doc["enabled"]         | true;
        c.dryRun        = doc["dry_run"]         | false;
        c.bootDelayMs   = doc["boot_delay_ms"]   | (uint32_t)20000;
        c.maxPerBoot    = doc["max_per_boot"]    | (uint32_t)4;
        c.cronIntervalS = doc["cron_interval_s"] | (uint32_t)1800;

        // Bestandsgeraete auf das neue Fenster heben. saveAutoModeConfig()
        // schreibt boot_delay_ms IMMER mit — wer je den Auto-Mode-Schalter
        // in der UI betaetigt hat, traegt den alten Default 10000 im Flash
        // und saehe von der zugesagten Verlaengerung sonst nichts (ein
        // reiner Struct-Default-Bump greift nur bei frischem NVS).
        // Angehoben wird ausschliesslich der EXAKTE Alt-Default: ein
        // bewusst abweichender Wert bleibt unangetastet. Der Wert ist
        // ohnehin nicht UI-exponiert, nur ueber PUT /api/automode setzbar
        // — ein gespeichertes 10000 ist praktisch immer der Altbestand.
        // Kein NVS-Write hier: die Anhebung gilt fuer diesen Lauf, der
        // naechste regulaere Save persistiert sie beilaeufig.
        if (c.bootDelayMs == 10000) c.bootDelayMs = 20000;
    }
    return c;
}

bool saveAutoModeConfig(const AutoModeConfig& cfg) {
    JsonDocument doc;
    doc["enabled"]         = cfg.enabled;
    doc["dry_run"]         = cfg.dryRun;
    doc["boot_delay_ms"]   = cfg.bootDelayMs;
    doc["max_per_boot"]    = cfg.maxPerBoot;
    doc["cron_interval_s"] = cfg.cronIntervalS;
    // Cleanup-Variante wie alle anderen User-Settings-Stores (preset_store,
    // speaker_inventory, ui_auth, spotify_player): bei vollem NVS werden erst
    // regenerable Caches gepurgt + retried, statt den Save still zu verschlucken.
    // Rueckgabe propagiert bis in den PUT-Handler (ehrliche UI-Antwort).
    return nvsSaveJsonWithCleanup(NVS_NS, NVS_KEY, doc);
}

void startAutoModeTask() {
    initMutex_();
    if (g_taskSpawned) return;
    g_taskSpawned = true;
    xTaskCreate(autoModeTask_, "auto-mode", 8192, nullptr,
                tskIDLE_PRIORITY + 1, nullptr);
}

AutoModeStatus getAutoModeStatus() {
    initMutex_();
    Lock_ l;
    AutoModeStatus snap = g_status;
    // Ableitung statt Per-Sekunden-Push: aus sleepStartMs + sleepDurMs
    // berechnen wie viel Sekunden bis zum naechsten Tick uebrig sind.
    if (snap.sleepDurMs > 0) {
        uint32_t elapsed = millis() - snap.sleepStartMs;
        snap.nextTickInS = (elapsed >= snap.sleepDurMs)
                            ? 0
                            : (snap.sleepDurMs - elapsed) / 1000;
    } else {
        snap.nextTickInS = 0;
    }
    return snap;
}

} // namespace sixback

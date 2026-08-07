// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ota_pull.h"

#ifdef SIXBACK_OTA_ENABLED

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace sixback {
namespace ota {

namespace {

// Manifest-Quelle. sixback.io ist seit 2026-05-25 die kanonische Domain
// (siehe reference_sixback_io_domain). Apache-vhost auf demselben Hetzner-Host
// wie install.busware.de — DocumentRoot /var/www/install/sixback/, also
// 1:1 Spiegel ohne /sixback/-Prefix.
constexpr const char* kManifestUrl =
    "https://sixback.io/manifest.json";

// Chip-spezifischer Datei-Name fuer firmware.bin oder littlefs.bin.
// build_release.sh erzeugt:
//   sixback-<tgt>-firmware.bin
//   sixback-<tgt>-littlefs.bin
// wobei <tgt> einer der platformio-envs ist (esp32, s3, s3-8mb, c3, c6).
//
// SIXBACK_OTA_PREFIX (platformio.ini build_flags) uebersteuert die Chip-
// Erkennung fuer Layout-Varianten desselben Chips: env:s3-8mb (Issue #23)
// MUSS "sixback-s3-8mb-" ziehen — das Standard-s3-littlefs ist 10 MB und
// passt nicht in die 1.875-MB-spiffs-Partition des 8-MB-Layouts.
const char* chipPrefix_() {
#if   defined(SIXBACK_OTA_PREFIX)
    return SIXBACK_OTA_PREFIX;
#elif CONFIG_IDF_TARGET_ESP32S3
    return "sixback-s3-";
#elif CONFIG_IDF_TARGET_ESP32C6
    return "sixback-c6-";
#elif CONFIG_IDF_TARGET_ESP32C3
    return "sixback-c3-";
#elif CONFIG_IDF_TARGET_ESP32
    return "sixback-esp32-";
#else
    return nullptr;  // unbekannte Plattform — Install verweigern
#endif
}

Status            g_status;
SemaphoreHandle_t g_mtx = nullptr;

// Vergleicht zwei Versions-Strings. Nur der fuehrende MAJOR.MINOR.PATCH-Kern
// zaehlt: sscanf("%d.%d.%d") stoppt am ersten Nicht-Ziffer-Zeichen, also wird
// das semver-Build-Metadata-Suffix eines Dev-Builds ("0.8.4+1116") ignoriert
// und vergleicht gleich zu seinem Basis-Release "0.8.4". Das ist Absicht — der
// Build-Counter liegt hinter dem '+' (siehe scripts/version_bump.py::
// _last_release_core), damit ein Dev-Build nie wie ein hoeherer Patch als das
// naechste Release aussieht (frueherer Bug: "0.8.1105" > "0.8.4" -> kein Update).
// Releases tragen einen sauberen "MAJOR.MINOR.PATCH"-Tag.
// Return: -1 wenn a < b, 0 wenn gleich, +1 wenn a > b, -2 bei Parse-Fehler.
int semverCompare_(const String& a, const String& b) {
    int aMaj=0, aMin=0, aBld=0, bMaj=0, bMin=0, bBld=0;
    int aGot = sscanf(a.c_str(), "%d.%d.%d", &aMaj, &aMin, &aBld);
    int bGot = sscanf(b.c_str(), "%d.%d.%d", &bMaj, &bMin, &bBld);
    if (aGot != 3 || bGot != 3) return -2;
    if (aMaj != bMaj) return aMaj < bMaj ? -1 : 1;
    if (aMin != bMin) return aMin < bMin ? -1 : 1;
    if (aBld != bBld) return aBld < bBld ? -1 : 1;
    return 0;
}

void lock_()   { if (g_mtx) xSemaphoreTake(g_mtx, portMAX_DELAY); }
void unlock_() { if (g_mtx) xSemaphoreGive(g_mtx); }

void setState_(State s) {
    lock_(); g_status.state = s; unlock_();
}

void setError_(const String& msg) {
    lock_();
    g_status.state = State::ERROR_;
    g_status.error = msg;
    unlock_();
    Serial.printf("[ota-pull] ERROR: %s\n", msg.c_str());
}

// Beziehe das Manifest und parse. Auf Erfolg: latest gesetzt + state
// AVAILABLE oder IDLE. Auf Fehler: state ERROR_.
void doCheck_() {
    WiFiClientSecure tls;
    tls.setInsecure();  // sixback.io hat ein gueltiges Let's-Encrypt-Cert, aber
                        // wir wollen keinen Cert-Bundle mit-flashen muessen.
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    if (!http.begin(tls, kManifestUrl)) {
        setError_("http.begin manifest failed");
        return;
    }
    int code = http.GET();
    if (code != 200) {
        String msg = "manifest HTTP "; msg += code;
        http.end();
        setError_(msg);
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    auto err = deserializeJson(doc, body);
    if (err) {
        String msg = "manifest parse: "; msg += err.c_str();
        setError_(msg);
        return;
    }
    String latest = doc["version"] | "";
    if (latest.length() == 0) {
        setError_("manifest has no 'version'");
        return;
    }

    // Semver-Vergleich. AVAILABLE nur wenn latest STRIKT NEUER als current —
    // Downgrades + Gleichstand werden zu IDLE. Bei Parse-Fehler Fallback auf
    // String-Inequality (alte Logik).
    int cmp = semverCompare_(g_status.current, latest);
    State next;
    const char* reason;
    if (cmp == -2) {
        next = (latest == g_status.current) ? State::IDLE : State::AVAILABLE;
        reason = (next == State::AVAILABLE) ? "diff (semver-parse-failed)" : "equal (semver-parse-failed)";
    } else if (cmp == 0) {
        next = State::IDLE;
        reason = "up-to-date";
    } else if (cmp < 0) {
        next = State::AVAILABLE;
        reason = "update available (latest > current)";
    } else {
        next = State::IDLE;
        reason = "current > latest — no downgrade";
    }
    lock_();
    g_status.latest = latest;
    g_status.error  = "";
    g_status.state  = next;
    unlock_();
    Serial.printf("[ota-pull] check: latest=%s current=%s cmp=%d -> %s (%s)\n",
                  latest.c_str(), g_status.current.c_str(), cmp,
                  next == State::AVAILABLE ? "AVAILABLE" : "IDLE",
                  reason);
}

// Pull eine Datei aus sixback.io + schreibe sie via Update.begin in den
// angegebenen Update-Bereich (U_FLASH = firmware-slot, U_SPIFFS =
// LittleFS-Partition). Setzt unterwegs g_status.progress + g_status.total.
// Return true on success.
bool pullAndFlashOne_(const char* path, int updateType,
                      const char* phaseName, uint8_t phaseIdx) {
    String url = "https://sixback.io/";
    url += path;
    Serial.printf("[ota-pull] phase %d/%u start: %s -> %s\n",
                  phaseIdx, (unsigned)g_status.phaseN, url.c_str(),
                  updateType == U_FLASH ? "FLASH" : "SPIFFS");

    lock_();
    g_status.phase    = phaseName;
    g_status.phaseIdx = phaseIdx;
    g_status.progress = 0;
    g_status.total    = 0;
    unlock_();

    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    if (!http.begin(tls, url)) {
        setError_(String(phaseName) + ": http.begin failed");
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        String msg = phaseName; msg += ": HTTP "; msg += code;
        http.end();
        setError_(msg);
        return false;
    }
    int contentLen = http.getSize();
    // 2026-05-28 (Fred-S3-OTA-Brick): Ohne verlaessliches Content-Length koennen
    // wir den Download nicht auf Vollstaendigkeit pruefen — der Truncation-Guard
    // weiter unten greift nur bei contentLen>0. Auf dem direkten Apache-Pfad
    // (sixback.io ist seit 2026-05-28 DNS-only, KEIN Cloudflare mehr) liefert der
    // Server IMMER ein Content-Length. Ein <=0 hiesse: eine Middlebox/CDN chunked't
    // den Stream — genau der Pfad, auf dem ein truncated Image committed + gebootet
    // wuerde (= Brick). Lieber hart abbrechen als blind committen.
    if (contentLen <= 0) {
        setError_(String(phaseName) +
                  ": no Content-Length (chunked/proxy?) — refusing to flash");
        http.end();
        return false;
    }
    // 2026-05-22 Pre-Release: empirisch sieht WiFiClientSecure-HTTPClient auf
    // C6 + S3 fuer einzelne .bin-Downloads ein Content-Length kleiner als
    // die Apache-tatsaechlich-Antwort (z.B. C6-firmware.bin: file=1822160 B,
    // ESP sah total=1797920 B = 24 KB short). Loop exit'd bei written>=total,
    // Update.end commit'te truncated Image → boot-image-magic-fail →
    // Rollback → device-brick. UPDATE_SIZE_UNKNOWN umgeht das: Stream bis
    // EOF/disconnect, Update finalisiert was tatsaechlich kam, partition-
    // boundary-check macht Update.cpp selbst.
    int total = UPDATE_SIZE_UNKNOWN;

    // 2026-06-07 (Issue #23, s3-8mb-Review): Pre-Flight-Groessencheck GEGEN DIE
    // ZIEL-PARTITION, und zwar VOR LittleFS.end()/Update.begin. Szenario: ein
    // Geraet mit 8-MB-Partition-Table laeuft (durch Fehlflash am falschen
    // Webflasher-Button) die Standard-s3-Firmware → chipPrefix_() zieht das
    // 10-MB-Standard-littlefs in die 1,9-MB-spiffs-Partition. Ohne diesen Check
    // wuerde erst das laufende FS unmountet und dann Update.write am Partition-
    // Ende scheitern — FS zerstoert, bevor der Fehler sichtbar wird. Generischer
    // Schutz: faengt auch server-seitig vertauschte Artefakte ab.
    {
        const esp_partition_t* target = (updateType == U_SPIFFS)
            ? esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL)
            : esp_ota_get_next_update_partition(NULL);
        if (target && (uint32_t)contentLen > target->size) {
            String msg = phaseName;
            msg += ": image ("; msg += contentLen;
            msg += " B) larger than target partition ("; msg += target->size;
            msg += " B) — wrong build variant for this board? refusing to flash";
            setError_(msg);
            http.end();
            return false;
        }
    }

    lock_();
    g_status.total = (contentLen > 0) ? (uint32_t)contentLen : 0;
    unlock_();
    Serial.printf("[ota-pull] phase %d GET 200, ContentLength=%d "
                  "(using UPDATE_SIZE_UNKNOWN for write)\n",
                  phaseIdx, contentLen);

    // FS muss vor U_SPIFFS-Write unmountet werden, sonst sehen wir die
    // alten Daten vom alten Mount + Heap-Pressure.
    if (updateType == U_SPIFFS) {
        LittleFS.end();
    }
    if (!Update.begin(total, updateType)) {
        setError_(String(phaseName) + ": Update.begin: " + Update.errorString());
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        setError_(String(phaseName) + ": no stream from HTTPClient");
        Update.abort();
        http.end();
        return false;
    }

    const size_t bufSz = 4096;
    uint8_t* buf = (uint8_t*)malloc(bufSz);
    if (!buf) {
        setError_(String(phaseName) + ": malloc buf failed");
        Update.abort();
        http.end();
        return false;
    }

    uint32_t written = 0;
    uint32_t lastReport = millis();
    uint32_t lastDataMs = millis();
    const uint32_t phaseStartMs = millis();
    // Read until connection closes OR no new bytes for kIdleTimeoutMs (idle-EOF).
    // Pre-Release-fix: NICHT auf written>=total brechen — total ist
    // (auf ESP HTTPS) unzuverlaessig.
    // 2026-05-28 (Fred-S3-OTA-Brick): 5s war fuer die ~10MB-FS-Phase des S3 zu
    // knapp — ein langsamer mbedTLS-Read ueber WiFi kann laenger als 5s stocken,
    // wodurch der Loop faelschlich "EOF" annimmt, der Truncation-Guard abbricht
    // und die spiffs-Partition halb-beschrieben + die UI zerstoert zurueckbleibt.
    // 30s gibt langsamen Geraeten Luft, ohne bei echtem Abriss ewig zu haengen
    // (http.connected() faengt den sauberen Close ohnehin sofort).
    const uint32_t kIdleTimeoutMs = 30000;
    // 2026-06-10 (FHEM #52 betateilchen, "seit 10 Minuten so"): der Idle-Timeout
    // oben faengt nur ein TOTAL stehengebliebenes Read ab. Ein pathologisch
    // langsamer-aber-lebendiger Link (oder ein half-open TCP, das connected()==true
    // haelt und gerade genug troepfelt um lastDataMs immer wieder zu resetten) kann
    // sonst BELIEBIG lange laufen, ohne dass je ein Fehler gesetzt wird. Wall-Clock-
    // Backstop: bei Ueberschreitung sauber abbrechen — Update.abort erhaelt die
    // Anti-Brick-Garantie (laufender Slot bleibt aktiv).
    // Budget SKALIERT mit der Image-Groesse (Mindest-Durchsatz ~3 KB/s als Untergrenze
    // dessen, was in vertretbarer Zeit fertig wird) statt flacher Decke — sonst koennte
    // das ~10-MB-s3-littlefs auf einem schwachen Link (RSSI-86-Population, #28) legitim
    // mitten im Fortschritt abgewuergt werden, waehrend ein 256-KB-Image eine enge Decke
    // verdient. Floor 5 min fuer kleine Images. Der 30-s-Idle-Timeout oben faengt echte
    // Total-Stalls weiterhin schnell; dieser Backstop nur den langsam-troepfelnden
    // half-open-Fall (contentLen ist hier garantiert > 0, oben per :206 erzwungen).
    constexpr uint32_t kMinBytesPerSec = 3000;
    uint32_t kPhaseMaxMs = (contentLen > 0)
        ? (uint32_t)((uint64_t)contentLen * 1000ULL / kMinBytesPerSec)
        : (20UL * 60UL * 1000UL);
    if (kPhaseMaxMs < 5UL * 60UL * 1000UL) kPhaseMaxMs = 5UL * 60UL * 1000UL;
    while (http.connected()) {
        const uint32_t nowMs     = millis();
        const uint32_t elapsedMs = nowMs - phaseStartMs;
        if (elapsedMs > kPhaseMaxMs) {
            free(buf);
            // 2026-08-06 (Lab, C6 auf dem Weg nach v0.8.41): dieser Guard hat bei
            // VOLLEM Tempo (~35 KB/s, >10x ueber den unterstellten 3 KB/s) nach
            // ~26 s gefeuert und dabei "exceeded 9 min" gemeldet — Meldung und
            // echte Laufzeit sind unvereinbar, die Ursache ist OFFEN (die
            // uint32-Differenz ist wrap-sicher, ein millis()-Ueberlauf erklaert es
            // nicht; ein Retry lief unter gleichen Bedingungen glatt durch).
            // Solange das ungeklaert ist, tragen wir die ROHEN Zaehler in der
            // Meldung mit: elapsed/budget/start/now zeigen beim naechsten Vorfall
            // sofort, ob wirklich Zeit vergangen ist oder der Vergleich luegt —
            // und zwar auch im Feld, wo niemand einen Serial-Mitschnitt hat.
            // setError_ loggt die Meldung ohnehin auf Serial, ein zweiter Print
            // waere Dopplung.
            setError_(String(phaseName) + ": phase exceeded " +
                      String(kPhaseMaxMs / 60000UL) +
                      " min wall-clock at " + String(written) + "/" +
                      String(contentLen) + " B — aborting (link too slow / stalled)" +
                      " [elapsed=" + String(elapsedMs) + "ms budget=" +
                      String(kPhaseMaxMs) + "ms start=" + String(phaseStartMs) +
                      " now=" + String(nowMs) + "]");
            Update.abort();
            http.end();
            return false;
        }
        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - lastDataMs > kIdleTimeoutMs) {
                Serial.printf("[ota-pull] phase %d idle >%lus — assume EOF at %u bytes\n",
                              phaseIdx, (unsigned long)(kIdleTimeoutMs / 1000),
                              (unsigned)written);
                break;
            }
            delay(5);
            continue;
        }
        int got = stream->readBytes(buf, std::min(avail, bufSz));
        if (got <= 0) break;
        if (Update.write(buf, got) != (size_t)got) {
            free(buf);
            setError_(String(phaseName) + ": Update.write: " + Update.errorString());
            Update.abort();
            http.end();
            return false;
        }
        written += got;
        lastDataMs = millis();
        if (millis() - lastReport > 500) {
            lock_();
            g_status.progress = written;
            unlock_();
            lastReport = millis();
        }
        if ((written & 0x3FFF) == 0) delay(1);
    }
    free(buf);
    http.end();

    // Wenn die HTTP-Antwort eine vernuenftig wirkende Content-Length hatte,
    // muss `written` auch nahe dran sein. Wenn deutlich kleiner (z.B. < 90 %),
    // war der Download offensichtlich abgebrochen → Update.end mit abort
    // statt commit, damit wir NICHT auf eine truncated Image-Partition
    // umschalten (= brick + rollback).
    if (contentLen > 0 && written < (uint32_t)(contentLen * 9 / 10)) {
        Serial.printf("[ota-pull] phase %d ABORT: only %u/%d bytes\n",
                      phaseIdx, (unsigned)written, contentLen);
        Update.abort();
        setError_(String(phaseName) + ": truncated download (" +
                  String(written) + "/" + String(contentLen) + ")");
        return false;
    }

    if (!Update.end(true)) {
        setError_(String(phaseName) + ": Update.end: " + Update.errorString());
        return false;
    }
    lock_();
    g_status.progress = written;
    if (g_status.total == 0) g_status.total = written;
    unlock_();
    Serial.printf("[ota-pull] phase %d DONE, %u bytes written "
                  "(announced %d).\n",
                  phaseIdx, (unsigned)written, contentLen);
    return true;
}

// Background-Task: Online-Install in 2 Phasen (FS + FW).
//   Phase 1: sixback-<chip>-littlefs.bin -> U_SPIFFS (Groesse je Target:
//            256 KB auf 4-MB-Layouts, 1.9 MB auf s3-8mb, ~10 MB auf s3-16MB)
//   Phase 2: sixback-<chip>-firmware.bin -> U_FLASH  (~1.7 MB)
//   -> ESP.restart()
// Reihenfolge: FS zuerst damit die NEUE UI nach Reboot sofort verfuegbar
// ist. Wenn FW-Flash fail (Update.end-Fehler), bleibt der Speaker auf
// app0 — alte Firmware + neue UI. Funktioniert weiter, war "graceful
// degradation" (die alte FW liefert die UI aus, neue Endpoints fehlen).
void installTask_(void* /*arg*/) {
    const char* prefix = chipPrefix_();
    if (!prefix) {
        setError_("unknown chip family");
        vTaskDelete(nullptr);
        return;
    }

    String fsName = prefix;  fsName += "littlefs.bin";
    String fwName = prefix;  fwName += "firmware.bin";

    if (!pullAndFlashOne_(fsName.c_str(), U_SPIFFS, "fs", 1)) {
        vTaskDelete(nullptr);
        return;
    }
    if (!pullAndFlashOne_(fwName.c_str(), U_FLASH,  "fw", 2)) {
        vTaskDelete(nullptr);
        return;
    }

    lock_();
    g_status.state = State::DONE;
    g_status.phase = "";
    unlock_();
    Serial.println("[ota-pull] both phases DONE. Reboot in 2s.");
    delay(2000);
    ESP.restart();
}

}  // anon

Status getStatus() {
    lock_();
    Status s = g_status;
    unlock_();
    return s;
}

void checkOnline() {
    setState_(State::CHECKING);
    lock_(); g_status.error = ""; unlock_();
    doCheck_();
}

namespace {
bool spawnInstallTask_() {
    // WICHTIG: state SOFORT (synchron, vor xTaskCreate) auf INSTALLING setzen.
    // Sonst hat der HTTP-Handler eine Race: er antwortet mit getStatus()-
    // Snapshot bevor der Task seine erste Anweisung ausgefuehrt hat — state
    // bleibt scheinbar idle/available, UI startet keinen Poll-Loop und
    // blendet den Progress-Bar nicht ein.
    lock_();
    g_status.state    = State::INSTALLING;
    g_status.progress = 0;
    g_status.total    = 0;
    g_status.error    = "";
    unlock_();

    BaseType_t r = xTaskCreate(installTask_, "ota-pull", 8192, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (r != pdPASS) {
        setError_("xTaskCreate failed");
        return false;
    }
    return true;
}
} // anon

bool installOnlineAsync() {
    // Laeuft schon ein Pull? Nicht erneut checken (TLS waehrend Flash = Heap-
    // Druck) und nicht doppelt spawnen.
    if (g_status.state == State::INSTALLING) {
        Serial.println("[ota-pull] installAsync rejected: already installing");
        return false;
    }
    // Selbst-validierend: liegt kein frisches AVAILABLE-Verdikt vor, JETZT
    // synchron nach-checken. Sonst blockt ein veralteter / abgelaufener / auf
    // ERROR gefallener vorheriger Check (oder gar keiner) einen legitimen
    // Install — der Install-Klick IST die Update-Absicht des Users, er soll
    // nicht erst manuell "Check" druecken muessen. (UX-Bug #6: das Gate meldete
    // faelschlich "no update available", obwohl ein Update vorlag — der Check-
    // State war zwischen Check und Install stale/ERROR geworden, u.a. weil ein
    // transienter TLS-Fehler unter Heap-Druck checkOnline() auf ERROR wirft.)
    if (g_status.state != State::AVAILABLE) {
        Serial.printf("[ota-pull] installAsync: state=%d not AVAILABLE — re-checking\n",
                      (int)g_status.state);
        doCheck_();   // synchron: setzt AVAILABLE / IDLE / ERROR
    }
    if (g_status.state != State::AVAILABLE) {
        Serial.printf("[ota-pull] installAsync rejected after re-check: state=%d\n",
                      (int)g_status.state);
        return false;
    }
    return spawnInstallTask_();
}

bool installOnlineForceAsync() {
    // INSTALLING-Lock pruefen damit wir nicht zwei Pull-Tasks parallel starten
    if (g_status.state == State::INSTALLING) {
        Serial.println("[ota-pull] forceInstall rejected: already installing");
        return false;
    }
    Serial.printf("[ota-pull] FORCE install requested (state was %d)\n",
                  (int)g_status.state);
    return spawnInstallTask_();
}

void init(const String& myVersion) {
    if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
    lock_();
    g_status.current = myVersion;
    g_status.state   = State::IDLE;
    unlock_();
}

}  // namespace ota
}  // namespace sixback

#else // SIXBACK_OTA_ENABLED

// No-op stubs damit api_endpoints/main weiterhin linken kann.
// C3/C6: kein OTA-pull weil partition-Schema single-app (kein A/B-Slot).
// Updates kommen via Web-Serial-Webflasher mit manifest-update.json.

#include <Arduino.h>

namespace sixback {
namespace ota {

void init(const String&) {}
Status getStatus() {
    Status s;
    s.state = State::ERROR_;
    s.error = "OTA disabled on this build — use Web-Serial-Updater";
    return s;
}
void checkOnline() {}
bool installOnlineAsync()      { return false; }
bool installOnlineForceAsync() { return false; }

} // namespace ota
} // namespace sixback

#endif // SIXBACK_OTA_ENABLED

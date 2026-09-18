// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "system_health.h"
#include "config.h"
#include "mono_clock.h"
#include "rom_delay_guard.h"
#include "systimer_blackbox.h"
#include "speaker_inventory.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_chip_info.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace sixback {

namespace {

// ------ Tunables -------------------------------------------------------------
constexpr uint32_t HEALTH_TWDT_S               = 30;                   // Task-WDT-Timeout
constexpr uint32_t HEALTH_WIFI_DOWN_REBOOT_S   = 5 * 60;               // 5 Min WLAN weg -> Reboot
constexpr uint32_t HEALTH_WIFI_RECONNECT_S     = 10;                   // alle 10s WiFi.reconnect()
constexpr uint32_t HEALTH_HEAP_LOW_BYTES       = 30 * 1024;            // <30 KB free
constexpr uint32_t HEALTH_HEAP_LOW_REBOOT_S    = 5 * 60;               // 5 Min unter Schwelle -> Reboot
constexpr uint32_t HEALTH_PING_INTERVAL_S      = 5 * 60;               // alle 5 Min Speaker pingen
constexpr uint8_t  HEALTH_PING_MISS_FOR_OFFLINE = 5;                   // 5x miss -> OFFLINE (5*5min = 25min stille bevor Status flippt)
constexpr uint16_t HEALTH_PING_TIMEOUT_MS       = 800;                 // GET /info Timeout
constexpr int32_t  HEALTH_TICK_DRIFT_EVENT_MS   = 50;                  // ab hier laufen die beiden Zeitbasen auseinander
// Absoluter Tick-Rueckstand statt Quotient. Der alte Quotient (dTick < dTimer/4)
// war ein Konstruktionsfehler: er skalierte die Schwelle mit dTimer und hat
// deshalb ZWEI echte Rueckstaende durchgelassen — [50,162] = 112 ms und
// [50,193] = 143 ms, beide um weniger als 2 ms verfehlt. Umgekehrt haette ein
// Counter-0-Vorwaertssprung als Riesen-"Stall" gezaehlt und die Statistik
// vergiftet.
// Untere Schwelle 20 ms: faengt beide verfehlten Faelle mit 5-7x Marge und
// liegt 10x ueber der einzigen Rauschquelle (2x 1 ms Quantisierung) — auf
// gesunder Hardware ist der Drift nachweislich exakt 0 (21 h auf dem
// Kontrollboard), ein Fehlalarm braeuchte 20 ms aus dem Nichts.
// Deckel 400 ms: ein UEBERLEBTER echter Tick-Rueckstand > ~300 ms kann es
// nicht geben, der INT_WDT feuert vorher (+100 ms fuer Intervall-Randlage).
// Alles darueber ist per Ausschluss ein Counter-0-Vorwaertssprung und gehoert
// in einen eigenen Zaehler, nicht in die Stall-Statistik.
constexpr int32_t  HEALTH_TICK_LAG_MIN_MS       = 20;
constexpr int32_t  HEALTH_TICK_LAG_MAX_MS       = 400;
constexpr uint8_t  HEALTH_DRIFT_RAW_SLOTS       = 16;                  // unbewertete (dTick,dTimer)-Paare der letzten Ereignisse

// ------ NVS-State ------------------------------------------------------------
constexpr const char* NVS_NS = "sixback-sys";

struct HealthState {
    uint32_t boot_count       = 0;
    uint32_t crash_count      = 0;
    uint32_t wifi_reboots     = 0;
    uint32_t heap_reboots     = 0;
    uint32_t wifi_disconnects = 0;   // erkannte WLAN-Abrisse (auch ohne Reboot)
    uint8_t  last_reset_raw   = 0;   // esp_reset_reason() vom letzten Boot
    bool     wdt_subscribed   = false;

    // Laufzeit-Tracking (kein NVS)
    uint32_t wifi_down_since_ms   = 0;   // 0 = WiFi up
    uint32_t wifi_last_reconnect_ms = 0;
    uint32_t last_wifi_down_s     = 0;   // Dauer des zuletzt abgeschlossenen Abrisses
    bool     wifi_ever_up         = false; // erst-Connect nicht als Abriss zaehlen
    uint32_t heap_low_since_ms    = 0;   // 0 = ok
    uint32_t heap_low_events      = 0;   // Episoden free-heap < Schwelle (runtime, KEIN NVS)
    uint32_t last_heap_low_s      = 0;   // uptime-s beim letzten heap-low-Event
    uint32_t last_ping_ms         = 0;

    // Sprungfeste Zeitbasis. ALLE Fristen dieses Moduls rechnen auf mono_ms,
    // nicht auf millis() — auf dem C6 springt millis() im Betrieb und hat
    // damit schon einen grundlosen Selbst-Reboot ausgeloest (siehe
    // mono_clock.h). mono_ms laeuft im Sprungfall stehen statt mitzuspringen.
    uint32_t mono_ms              = 0;   // monoton, sprungbereinigt
    uint32_t clock_jumps          = 0;   // verworfene Zeitspruenge seit Boot
    uint32_t last_clock_jump_s    = 0;   // mono-Zeitpunkt des letzten Sprungs

    // Zweite Zeitbasis-Diagnose: OS-Tick gegen esp_timer. clock_jumps oben
    // sieht NUR den esp_timer-Zaehler — beide INT_WDT-Signaturen des C6
    // rev v0.0 haengen aber am Tick-Zaehler und sind fuer ihn unsichtbar.
    int32_t  tick_drift_ms        = 0;   // kumuliert: Tick-Uhr minus esp_timer-Uhr
    int32_t  tick_drift_last_ms   = 0;   // Auseinanderlaufen beim letzten Ereignis
    uint32_t tick_drift_events    = 0;   // Intervalle ueber der Schwelle
    uint32_t tick_stall_max_ms    = 0;   // groesster UEBERLEBTER Tick-Rueckstand (Name/Semantik bewusst
                                         // beibehalten: Sampler und CSV-Spalten haengen daran)
    uint32_t tick_lag_events      = 0;   // wie oft ein Rueckstand >= HEALTH_TICK_LAG_MIN_MS auftrat
    uint32_t timer_fwd_jump_events = 0;  // Counter-0-VORWAERTSspruenge, getrennt gezaehlt
    uint32_t last_tick_drift_s    = 0;   // mono-Zeitpunkt des letzten Ereignisses
    // Rohwerte der letzten Ereignisse, unbewertet. Siehe Kommentar in
    // healthToJson: der Klassifikator hat am 25.08. einen Grenzfall
    // (dTick=50, dTimer=162) knapp verfehlt, der hinterher nicht mehr
    // aufloesbar war. Ohne Rohwerte ist "stall_max = 0" nicht dasselbe wie
    // "kein Tick-Rueckstand".
    struct DriftRaw { int32_t d_tick; int32_t d_timer; };
    DriftRaw drift_raw[HEALTH_DRIFT_RAW_SLOTS] = {};
    uint8_t  drift_raw_w          = 0;   // naechster Schreibindex
    uint8_t  drift_raw_n          = 0;   // wieviele Slots belegt (deckelt auf SLOTS)
    // Miss-Counter pro Speaker via Map (deviceId -> u8). Da wir nicht viele
    // haben, simple std::map waere ok, aber wir bleiben minimalistisch und
    // halten die Werte direkt am Speaker-Objekt nicht — wir verwalten sie
    // hier in einer kleinen statischen Tabelle (max 16 Speaker).
    struct PingMiss { String id; uint8_t miss; };
    PingMiss ping_misses[16];
} g;

const char* resetReasonText(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_UNKNOWN:    return "UNKNOWN";
        case ESP_RST_POWERON:    return "POWERON";
        case ESP_RST_EXT:        return "EXT";
        case ESP_RST_SW:         return "SW";
        case ESP_RST_PANIC:      return "PANIC";
        case ESP_RST_INT_WDT:    return "INT_WDT";
        case ESP_RST_TASK_WDT:   return "TASK_WDT";
        case ESP_RST_WDT:        return "WDT";
        case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:   return "BROWNOUT";
        case ESP_RST_SDIO:       return "SDIO";
        // Ab hier die Gruende, die IDF nach der 4.x-Aera dazubekommen hat. Ohne
        // sie meldete /api/status ein nichtssagendes "?" — z.B. nach jedem
        // esptool-Zugriff ueber den nativen USB-Serial-JTAG (ESP_RST_JTAG).
        case ESP_RST_USB:        return "USB";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default:                 return "?";
    }
}

bool isCrashReason(esp_reset_reason_t r) {
    // Zaehlt in crash_count. Massstab: der Neustart war NICHT gewollt.
    //   CPU_LOCKUP = Double Exception, also ein echter Absturz — fehlte hier
    //   und wurde deshalb ueberhaupt nicht gezaehlt.
    //   PWR_GLITCH/EFUSE sind Hardware-Fehlerereignisse in derselben Klasse
    //   wie das bereits gezaehlte BROWNOUT.
    // NICHT enthalten: USB/JTAG — die loest der Host beim Flashen/Debuggen aus,
    // sie als Absturz zu zaehlen wuerde den Zaehler im Lab unbrauchbar machen.
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
           r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
           r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP ||
           r == ESP_RST_PWR_GLITCH || r == ESP_RST_EFUSE;
}

uint8_t& pingMissForId(const String& id) {
    static uint8_t dummy = 0;
    // existierenden Slot finden
    for (auto& p : g.ping_misses) {
        if (p.id == id) return p.miss;
    }
    // freien Slot finden
    for (auto& p : g.ping_misses) {
        if (p.id.length() == 0) { p.id = id; p.miss = 0; return p.miss; }
    }
    return dummy;  // Tabelle voll - never mind
}

void resetPingMiss(const String& id) { pingMissForId(id) = 0; }

// Einmal je Tick: die sprungbereinigte Uhr weiterstellen. Ein verworfener
// Schritt wird laut gemeldet — auf einem gesunden Chip darf das nie passieren,
// und im Feld ist der Zaehler der einzige Hinweis darauf, dass die Zeitbasis
// des Boards nicht taugt.
void monoAdvance() {
    static MonoClock clk(millis());
    // Sockel: die Zeit zwischen Boot und dem ersten Tick (Setup, WiFi-Connect,
    // LittleFS-Mount) ist echte Uptime und darf nicht fehlen — sonst meldet
    // uptime_s dauerhaft ein paar Sekunden zu wenig. Nur beim allerersten
    // Aufruf, danach zaehlt ausschliesslich der geprueft plausible Zuwachs.
    static bool seeded = false;
    if (!seeded) { seeded = true; g.mono_ms = millis(); }
    const uint16_t before = clk.jumps();
    g.mono_ms += clk.step(millis());
    if (clk.jumps() != before) {
        ++g.clock_jumps;
        g.last_clock_jump_s = g.mono_ms / 1000;
        Serial.printf("[health] clock jump #%u verworfen — Zeitbasis unplausibel "
                      "(mono=%lus, millis=%lus)\n",
                      g.clock_jumps, (unsigned long)(g.mono_ms / 1000),
                      (unsigned long)(millis() / 1000));
    }
}

// Einmal je Tick: die beiden Hardware-Zeitbasen gegeneinander halten.
// WARUM: esp_timer/millis() laufen auf SYSTIMER-Counter 0, der FreeRTOS-Tick
// auf Counter 1 (esp_private/systimer.h). Der INT_WDT wird ausschliesslich vom
// Tick gefuettert — int_wdt.c registriert seinen wdt_hal_feed() als Tick-Hook —
// also heisst jeder INT_WDT-Panic woertlich "Counter 1 hat 300 ms nicht
// geliefert". Genau dafuer ist clock_jumps blind, weil es nur Counter 0
// abtastet: am 24.08. panicte ein Boot mit clock_jumps == 0. Hier laufen beide
// Zaehler gegeneinander, damit ein Zeitbasis-Fehler sichtbar wird, BEVOR der
// Watchdog zuschlaegt. Die Probe misst nur und greift nirgends ein.
void tickDriftAdvance() {
    static uint32_t prevTick  = 0;
    static uint32_t prevTimer = 0;
    static bool     seeded    = false;

    const uint32_t tickMs  = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    const uint32_t timerMs = (uint32_t)(esp_timer_get_time() / 1000);
    if (!seeded) { seeded = true; prevTick = tickMs; prevTimer = timerMs; return; }

    // Signierte Differenz wie in MonoClock::step: ein Rueckwaertssprung wird
    // negativ, der 32-Bit-Ueberlauf nach ~49 Tagen bleibt klein und positiv.
    const int32_t dTick  = (int32_t)(tickMs  - prevTick);
    const int32_t dTimer = (int32_t)(timerMs - prevTimer);
    prevTick  = tickMs;
    prevTimer = timerMs;

    // Auf gesunder Hardware laufen beide Zaehler auf derselben Taktquelle und
    // dTick == dTimer bis auf die 1-ms-Quantisierung des Ticks.
    const int32_t delta = dTick - dTimer;   // >0 Tick eilt vor, <0 Tick bleibt zurueck
    g.tick_drift_ms += delta;

    // Rohwerte mitschreiben, BEVOR klassifiziert wird. Aufgezeichnet wird
    // jedes Intervall, dessen Betrag ueber der halben Ereignisschwelle liegt —
    // damit landen auch die Faelle im Puffer, die weder als Drift-Ereignis
    // noch als Stall durchgehen, und genau die waren bisher unauswertbar.
    if (delta > HEALTH_TICK_DRIFT_EVENT_MS / 2 || delta < -HEALTH_TICK_DRIFT_EVENT_MS / 2) {
        g.drift_raw[g.drift_raw_w] = { dTick, dTimer };
        g.drift_raw_w = (uint8_t)((g.drift_raw_w + 1) % HEALTH_DRIFT_RAW_SLOTS);
        if (g.drift_raw_n < HEALTH_DRIFT_RAW_SLOTS) ++g.drift_raw_n;
    }

    if (delta > HEALTH_TICK_DRIFT_EVENT_MS || delta < -HEALTH_TICK_DRIFT_EVENT_MS) {
        ++g.tick_drift_events;
        g.tick_drift_last_ms = delta;
        g.last_tick_drift_s  = g.mono_ms / 1000;
        Serial.printf("[health] tick drift #%u — Tick vs esp_timer %+ld ms in diesem "
                      "Intervall (kumuliert %+ld ms, dTick=%ld dTimer=%ld)\n",
                      g.tick_drift_events, (long)delta, (long)g.tick_drift_ms,
                      (long)dTick, (long)dTimer);
    }

    // Der Tick stand, waehrend die Wanduhr weiterlief — der Zustand, aus dem
    // heraus der INT_WDT feuert. Unter 300 ms ueberlebt das Board es, und genau
    // diese ueberlebten Faelle sind der Beleg, den ein Panic-Dump nicht liefert.
    const int32_t lag = dTimer - dTick;   // >0: Tick blieb hinter der Wanduhr zurueck
    if (dTick >= 0 && dTimer > 0 && lag >= HEALTH_TICK_LAG_MIN_MS && lag <= HEALTH_TICK_LAG_MAX_MS) {
        ++g.tick_lag_events;
        if ((uint32_t)lag > g.tick_stall_max_ms) g.tick_stall_max_ms = (uint32_t)lag;
        Serial.printf("[health] tick lag — OS-Tick %ld ms hinter esp_timer (dTick=%ld dTimer=%ld)\n",
                      (long)lag, (long)dTick, (long)dTimer);
    } else if (dTick >= 0 && dTimer > 0 && lag > HEALTH_TICK_LAG_MAX_MS) {
        ++g.timer_fwd_jump_events;
    }
    // ACHTUNG, ehrliche Grenze: aus loop() heraus ist [50,193] nicht von einem
    // kleinen Counter-0-VORWAERTSsprung zu unterscheiden — beide sehen gleich
    // aus. Eindeutig misst nur der Blackbox-ISR (tick_freeze_events dort).
}

void persistCounters() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putUInt("boot_count",   g.boot_count);
    p.putUInt("crash_count",  g.crash_count);
    p.putUInt("wifi_reboots", g.wifi_reboots);
    p.putUInt("heap_reboots", g.heap_reboots);
    p.putUInt("wifi_disc",    g.wifi_disconnects);
    p.putUChar("last_reason", g.last_reset_raw);
    p.end();
}

void doSelfReboot(const char* reason, uint32_t& counterField) {
    Serial.printf("[health] SELF-REBOOT: %s\n", reason);
    ++counterField;
    persistCounters();
    // Kein delay() vor dem Neustart: delay() haengt an derselben Zeitbasis,
    // die uns hier ueberhaupt erst in den Reboot getrieben haben kann. Der
    // Selbst-Reboot vom 2026-08-20 endete zwischen dieser Zeile und dem
    // Neustart im INT_WDT (rst:0xc statt sauberem SW-Reset). Serial.flush()
    // leert den TX-Puffer ohne Uhr.
    Serial.flush();
    ESP.restart();
}

// Single-Speaker self-ping (synchron, 800ms timeout - billig).
// Mutiert die Kopie `s`; statusChanged=true wenn der Caller nach dem Merge
// saveToNVS aufrufen soll. saveToNVS wird hier NICHT mehr direkt gemacht —
// sonst persistieren wir den vor-merge-Zustand mit stalen Feldern.
void pingOneSpeaker(Speaker& s, bool& statusChanged) {
    statusChanged = false;
    if (s.ip.length() == 0) return;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(HEALTH_PING_TIMEOUT_MS);
    http.setTimeout(HEALTH_PING_TIMEOUT_MS);
    String url = "http://" + s.ip + ":" + String(BOSE_BMX_PORT) + "/info";
    if (!http.begin(url)) return;
    int code = http.GET();
    http.end();
    if (code == 200) {
        s.lastSeenMs = millis();
        resetPingMiss(s.deviceId);
        if (s.status == MigrationStatus::OFFLINE) {
            // war offline, jetzt wieder da - status zurueck auf UNKNOWN damit
            // ein refresh-status den echten Stand holt
            s.status = MigrationStatus::UNKNOWN;
            statusChanged = true;
        }
    } else {
        uint8_t& miss = pingMissForId(s.deviceId);
        if (miss < 255) ++miss;
        if (miss >= HEALTH_PING_MISS_FOR_OFFLINE &&
            s.status != MigrationStatus::OFFLINE) {
            Serial.printf("[health] speaker %s (%s) OFFLINE after %u misses\n",
                          s.deviceId.c_str(), s.ip.c_str(), miss);
            s.status = MigrationStatus::OFFLINE;
            statusChanged = true;
        }
    }
}

void pingAllSpeakers() {
    auto& inv = SpeakerInventory::instance();
    // Snapshot OHNE Lock fuer HTTP-IO, dann unter Lock zurueck-mergen +
    // saveToNVS nur wenn sich tatsaechlich Status aenderte.
    auto snapshot = inv.list();
    bool anyChange = false;
    for (auto& s : snapshot) {
        bool changed = false;
        pingOneSpeaker(s, changed);
        SpeakerInventory::LockGuard g(inv);
        if (auto* live = inv.findById(s.deviceId)) {
            live->lastSeenMs = s.lastSeenMs;
            live->status     = s.status;
        }
        if (changed) anyChange = true;
    }
    if (anyChange) inv.saveToNVS();
}

}  // namespace

void healthInit() {
    Preferences p;
    if (p.begin(NVS_NS, false)) {
        g.boot_count     = p.getUInt("boot_count",   0);
        g.crash_count    = p.getUInt("crash_count",  0);
        g.wifi_reboots   = p.getUInt("wifi_reboots", 0);
        g.heap_reboots   = p.getUInt("heap_reboots", 0);
        g.wifi_disconnects = p.getUInt("wifi_disc", 0);
        g.last_reset_raw = p.getUChar("last_reason", 0);
        p.end();
    }
    esp_reset_reason_t r = esp_reset_reason();
    if (isCrashReason(r)) ++g.crash_count;
    ++g.boot_count;
    g.last_reset_raw = (uint8_t)r;
    persistCounters();

    Serial.printf("[health] boot #%u  last-reset=%s  crashes-lifetime=%u  "
                  "wifi-reboots=%u  heap-reboots=%u\n",
                  g.boot_count, resetReasonText(r),
                  g.crash_count, g.wifi_reboots, g.heap_reboots);

    // Task-WDT subscriben (Loop-Task = aktuelle Task).
    // arduino-esp32 3.x: esp_task_wdt_init() existiert bereits mit
    // Default-Config; wir adden uns nur dazu. esp_task_wdt_add(NULL)
    // wirft ESP_ERR_INVALID_STATE wenn TWDT noch nicht init'd ist —
    // in dem Fall holen wir das nach.
    esp_err_t e = esp_task_wdt_add(NULL);
    if (e == ESP_ERR_INVALID_STATE) {
        // TWDT war noch nicht initialisiert (zB CONFIG_ESP_TASK_WDT_INIT=n).
        // Wir initialisieren ihn jetzt nur fuer uns.
        esp_task_wdt_config_t cfg = {
            .timeout_ms   = HEALTH_TWDT_S * 1000,
            .idle_core_mask = 0,    // Idle-Task nicht beobachten
            .trigger_panic = true,
        };
        if (esp_task_wdt_init(&cfg) == ESP_OK) {
            e = esp_task_wdt_add(NULL);
        }
    } else if (e == ESP_OK) {
        // schon initialisiert mit fremder Config — wir koennen Timeout NICHT
        // unilateral umstellen. Default in arduino-esp32 ist meist 5s,
        // damit muessten wir < 5s ticken. Wir versuchen ein reconfigure.
        esp_task_wdt_config_t cfg = {
            .timeout_ms   = HEALTH_TWDT_S * 1000,
            .idle_core_mask = 0,
            .trigger_panic = true,
        };
        esp_task_wdt_reconfigure(&cfg);
    }
    g.wdt_subscribed = (e == ESP_OK || e == ESP_ERR_INVALID_STATE) &&
                       esp_task_wdt_status(NULL) == ESP_OK;
    Serial.printf("[health] task-wdt subscribed=%d (timeout=%us)\n",
                  g.wdt_subscribed ? 1 : 0, HEALTH_TWDT_S);
}

void healthTick() {
    // NICHT millis(): siehe mono_clock.h. Alles unterhalb rechnet auf der
    // sprungbereinigten Uhr, damit ein Zeitsprung keine Frist zum Feuern
    // bringt, die real noch gar nicht abgelaufen ist.
    monoAdvance();
    tickDriftAdvance();
    // Zieht die Ring-Kopie, wenn der Blackbox-ISR nach einem ueberlebten
    // Tick-Stillstand einen Snapshot angefordert hat (No-Op ohne Build-Flag).
    blackboxFreezeService();
    const uint32_t now = g.mono_ms;

    // 1) Task-WDT feeden
    if (g.wdt_subscribed) esp_task_wdt_reset();

    // 2) WiFi-Watchdog
    // "Verbunden" heisst WL_CONNECTED UND gueltige IP. Ein STA kann
    // WL_CONNECTED bei 0.0.0.0 halten (stiller Drop / DHCP-Renew-Fail) —
    // eine reine status()-Pruefung sieht das nicht und der Watchdog wuerde
    // nie armen. Deshalb die IP mitpruefen (globale WLAN-Checkliste).
    bool wifi = (WiFi.status() == WL_CONNECTED) && (uint32_t)WiFi.localIP() != 0;
    if (wifi) {
        if (g.wifi_down_since_ms != 0) {
            // war down, jetzt wieder da — Dauer des Abrisses festhalten
            g.last_wifi_down_s = (now - g.wifi_down_since_ms) / 1000;
            Serial.printf("[health] WiFi recovered after %lus\n",
                          (unsigned long)g.last_wifi_down_s);
        }
        g.wifi_down_since_ms = 0;
        g.wifi_ever_up = true;
    } else {
        if (g.wifi_down_since_ms == 0) {
            g.wifi_down_since_ms = now;
            g.wifi_last_reconnect_ms = 0;
            // Erst-Connect nach dem Boot (noch nie up) NICHT als Abriss zaehlen.
            if (g.wifi_ever_up) {
                ++g.wifi_disconnects;
                persistCounters();
            }
            Serial.printf("[health] WiFi disconnected — watchdog armed "
                          "(disconnect #%u)\n", g.wifi_disconnects);
        }
        // Periodisch reconnect anstossen (nicht im Sekundentakt - alle 10s)
        if (now - g.wifi_last_reconnect_ms > HEALTH_WIFI_RECONNECT_S * 1000) {
            g.wifi_last_reconnect_ms = now;
            Serial.printf("[health] WiFi.reconnect() (down for %lus)\n",
                          (now - g.wifi_down_since_ms) / 1000);
            WiFi.reconnect();
        }
        if (now - g.wifi_down_since_ms > HEALTH_WIFI_DOWN_REBOOT_S * 1000) {
            doSelfReboot("WiFi down > threshold", g.wifi_reboots);
        }
    }

    // 3) Heap-Watchdog
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < HEALTH_HEAP_LOW_BYTES) {
        if (g.heap_low_since_ms == 0) {
            g.heap_low_since_ms = now;
            ++g.heap_low_events;                       // FHEM 144729: Feld-Beleg der
            g.last_heap_low_s = g.mono_ms / 1000;      // Heap-Druck-Episode (auch ohne Reboot)
            Serial.printf("[health] free-heap %u < %u — watchdog armed (event #%u)\n",
                          freeHeap, HEALTH_HEAP_LOW_BYTES, g.heap_low_events);
        }
        if (now - g.heap_low_since_ms > HEALTH_HEAP_LOW_REBOOT_S * 1000) {
            doSelfReboot("free-heap low > threshold", g.heap_reboots);
        }
    } else {
        if (g.heap_low_since_ms != 0) {
            Serial.printf("[health] free-heap recovered (%u) — disarm\n", freeHeap);
        }
        g.heap_low_since_ms = 0;
    }

    // 4) Self-Ping (nur wenn WiFi up)
    if (wifi && (now - g.last_ping_ms > HEALTH_PING_INTERVAL_S * 1000)) {
        g.last_ping_ms = now;
        pingAllSpeakers();
    }
}

void healthToJson(JsonObject out) {
    out["boot_count"]      = g.boot_count;
    out["crash_count"]     = g.crash_count;
    out["wifi_reboots"]    = g.wifi_reboots;
    out["heap_reboots"]    = g.heap_reboots;
    out["wifi_disconnects"] = g.wifi_disconnects;
    out["last_wifi_down_s"] = g.last_wifi_down_s;
    out["last_reset"]      = resetReasonText((esp_reset_reason_t)g.last_reset_raw);
    out["wdt_subscribed"]  = g.wdt_subscribed;
    out["wifi_down_for_s"] = g.wifi_down_since_ms == 0 ? 0
                              : (g.mono_ms - g.wifi_down_since_ms) / 1000;
    out["heap_low_for_s"]  = g.heap_low_since_ms == 0 ? 0
                              : (g.mono_ms - g.heap_low_since_ms) / 1000;
    out["heap_low_events"] = g.heap_low_events;
    out["last_heap_low_s"] = g.last_heap_low_s;
    out["last_ping_age_s"] = g.last_ping_ms == 0 ? -1
                              : (int)((g.mono_ms - g.last_ping_ms) / 1000);
    // Zeitbasis-Diagnose. uptime_s in /api/status ist seit v0.8.46 die
    // sprungbereinigte Zeit; uptime_raw_s daneben ist rohes millis()/1000.
    // Divergieren die beiden, hat die Uhr des Boards gesprungen — damit meldet
    // jedes Geraet im Feld selbst, ob es betroffen ist (beides steht auch im
    // Diagnose-Snapshot).
    out["uptime_mono_s"]     = g.mono_ms / 1000;
    out["uptime_raw_s"]      = millis() / 1000;
    out["clock_jumps"]       = g.clock_jumps;
    out["last_clock_jump_s"] = g.last_clock_jump_s;
    // Verworfene unplausible Busy-Waits (src/rom_delay_guard.cpp). Bleibt 0,
    // wo der Guard nicht eingebaut ist. Steigt er, hat der Deckel gerade einen
    // INT_WDT-Panic verhindert — last_us ist das angeforderte Delay.
    out["rom_delay_clamped"] = romDelayClamped();
    out["rom_delay_last_us"] = romDelayLastClampedUs();
    // Zweite Zeitbasis: OS-Tick gegen esp_timer. Bleibt 0, solange beide
    // SYSTIMER-Zaehler synchron laufen. Steigt tick_drift_events oder
    // tick_stall_max_ms, ist der Tick-Zaehler gestoert — die Zeitbasis, an der
    // die INT_WDT-Panics haengen und die clock_jumps oben nicht sieht.
    out["tick_drift_ms"]      = g.tick_drift_ms;
    out["tick_drift_last_ms"] = g.tick_drift_last_ms;
    out["tick_drift_events"]  = g.tick_drift_events;
    out["tick_stall_max_ms"]       = g.tick_stall_max_ms;
    out["tick_lag_events"]         = g.tick_lag_events;
    out["timer_fwd_jump_events"]   = g.timer_fwd_jump_events;
    out["last_tick_drift_s"]  = g.last_tick_drift_s;
    // Rohwert-Paare der letzten Ereignisse. Die Schwelle oben klassifiziert,
    // und ein Grenzfall faellt sonst unter den Tisch: am 25.08. lag ein
    // Ereignis mit dTick=50/dTimer=162 knapp neben dem Stall-Kriterium
    // (50 > 162/4) und war hinterher nicht mehr aufloesbar. Hier stehen die
    // unbewerteten Paare, damit die Einordnung nicht am Klassifikator haengt.
    JsonArray raw = out["tick_drift_raw"].to<JsonArray>();
    const uint8_t n = g.drift_raw_n < HEALTH_DRIFT_RAW_SLOTS
                        ? g.drift_raw_n : HEALTH_DRIFT_RAW_SLOTS;
    for (uint8_t i = 0; i < n; ++i) {
        const uint8_t idx = (uint8_t)((g.drift_raw_w + HEALTH_DRIFT_RAW_SLOTS - n + i)
                                      % HEALTH_DRIFT_RAW_SLOTS);
        JsonArray p = raw.add<JsonArray>();
        p.add(g.drift_raw[idx].d_tick);
        p.add(g.drift_raw[idx].d_timer);
    }
    blackboxToJson(out);
}

uint32_t monoUptimeS() { return g.mono_ms / 1000; }

const char* lastResetReasonStr() {
    return resetReasonText((esp_reset_reason_t)g.last_reset_raw);
}

const char* chipModelStr() {
    esp_chip_info_t info; esp_chip_info(&info);
    return info.model == CHIP_ESP32S3 ? "ESP32-S3" :
           info.model == CHIP_ESP32   ? "ESP32"    :
           info.model == CHIP_ESP32C3 ? "ESP32-C3" :
           info.model == CHIP_ESP32C6 ? "ESP32-C6" :
           info.model == CHIP_ESP32C5 ? "ESP32-C5" : "?";
}

}  // namespace sixback

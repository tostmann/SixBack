// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — SYSTIMER-Blackbox. Siehe systimer_blackbox.h fuer das Warum.

#include "systimer_blackbox.h"

#if defined(SIXBACK_SYSTIMER_BLACKBOX)

#include <Arduino.h>
#include "system_health.h"   // monoUptimeS()
#include <cstdarg>
#include <cstring>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_intr_alloc.h>
#include <esp_system.h>
#include <driver/gptimer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace sixback {
namespace {

// ---------------------------------------------------------------------------
// SYSTIMER-Register. Adressen aus dem verifizierten IDF-Header
// components/soc/esp32c6/include/soc/systimer_reg.h (DR_REG_SYSTIMER_BASE
// 0x6000A000) — nicht aus dem Gedaechtnis. Der praeprozessierte Header liegt
// im Arduino-libs-Baum nicht bei (soc/systimer_struct.h fehlt dort), deshalb
// hier als Konstanten statt per #include; es sind Hardware-Adressen, die sich
// ueber IDF-Versionen nicht aendern.
// Counter-Zuordnung aus esp_private/systimer.h:
//   Counter 0 = esptimer (millis/esp_timer_get_time)
//   Counter 1 = OS-Tick  (fuettert den INT_WDT)
//   Alarm   0 = OS-Tick Core 0
// ---------------------------------------------------------------------------
constexpr uint32_t SYSTIMER_BASE     = 0x6000A000u;
constexpr uint32_t REG_UNIT0_OP      = SYSTIMER_BASE + 0x04;
constexpr uint32_t REG_UNIT1_OP      = SYSTIMER_BASE + 0x08;
// TARGET0_HI/LO (+0x1C/+0x20) sind hier NUTZLOS und lesen konstant 0 — kein
// Lesefehler, sondern die Betriebsart: der OS-Tick-Alarm laeuft im PERIODEN-
// modus. Belegt: freertos/port_systick.c:98-103 konfiguriert Alarm 0 per
// systimer_hal_set_alarm_period() + SYSTIMER_ALARM_MODE_PERIOD; hal/
// systimer_hal.c:114-120 -> systimer_ll_set_alarm_period() (systimer_ll.h:155)
// schreibt AUSSCHLIESSLICH das 26-bit-Feld TARGET0_CONF.period. Die
// TARGET_HI/LO-Register beschreibt nur systimer_ll_set_alarm_target() aus dem
// ONESHOT-Pfad — den nutzt esp_timer auf Alarm 2, nicht der Tick.
// Der tatsaechliche Komparatorwert steht in eigenen RO-Registern
// ("comp0 actual target value", systimer_reg.h:554/566) — die schreibt die
// Hardware im Periodenmodus selbst fort.
constexpr uint32_t REG_REAL_TGT0_LO  = SYSTIMER_BASE + 0x74;   // RO, 32 bit
constexpr uint32_t REG_REAL_TGT0_HI  = SYSTIMER_BASE + 0x78;   // RO, 20 bit
constexpr uint32_t REG_TARGET0_CONF  = SYSTIMER_BASE + 0x34;   // [25:0] period, [30] period_mode
constexpr uint32_t REG_CONF          = SYSTIMER_BASE + 0x00;   // [24] TARGET0_WORK_EN, [29] UNIT1_WORK_EN
constexpr uint32_t REG_INT_ENA       = SYSTIMER_BASE + 0x64;   // [0] TARGET0_INT_ENA
constexpr uint32_t REG_INT_RAW       = SYSTIMER_BASE + 0x68;   // [0] TARGET0_INT_RAW
constexpr uint32_t REG_UNIT0_VAL_HI  = SYSTIMER_BASE + 0x40;
constexpr uint32_t REG_UNIT0_VAL_LO  = SYSTIMER_BASE + 0x44;
constexpr uint32_t REG_UNIT1_VAL_HI  = SYSTIMER_BASE + 0x48;
constexpr uint32_t REG_UNIT1_VAL_LO  = SYSTIMER_BASE + 0x4C;

constexpr uint32_t BIT_VALUE_VALID   = (1u << 29);   // SYSTIMER_TIMER_UNITx_VALUE_VALID
constexpr uint32_t BIT_UPDATE        = (1u << 30);   // SYSTIMER_TIMER_UNITx_UPDATE

// Deckel fuer den VALUE_VALID-Poll. Der Kern laeuft mit 160 MHz; ein gesunder
// Handshake ist in wenigen Zyklen fertig. 16000 Zyklen = 100 us sind grosszuegig
// und liegen weit unter dem 300-ms-Limit des INT_WDT, d.h. der Deckel kann den
// Panic nicht selbst ausloesen. Zyklenzaehler statt SYSTIMER, weil genau der
// SYSTIMER hier der Verdaechtige ist.
constexpr uint32_t ACK_TIMEOUT_CYCLES = 16000u;

constexpr uint16_t ACK_TIMEOUT_MARK   = 0xFFFFu;   // Handshake nicht gekommen
constexpr uint16_t ACK_CAP            = 0xFFFEu;

constexpr uint16_t FLAG_TIMEOUT_U0 = 0x0001;
constexpr uint16_t FLAG_TIMEOUT_U1 = 0x0002;
constexpr uint16_t FLAG_TORN_U0    = 0x0004;   // HI aenderte sich zwischen zwei Reads
constexpr uint16_t FLAG_TORN_U1    = 0x0008;
// Statusbits, im selben uint16 mitgefuehrt (kosten kein zusaetzliches Feld).
// Alle vier muessen im Normalbetrieb 1 sein; INT_RAW muss im Todesfenster 0
// bleiben — feuert der Alarm doch, ist das Alarm-Ausbleiben-Modell falsch.
constexpr uint16_t FLAG_INT_RAW    = 0x0010;   // TARGET0-Interrupt steht an
constexpr uint16_t FLAG_INT_ENA    = 0x0020;   // TARGET0-Interrupt freigegeben
constexpr uint16_t FLAG_PERIOD_MD  = 0x0040;   // TARGET0_CONF.period_mode
constexpr uint16_t FLAG_TGT0_WORK  = 0x0080;   // CONF.TARGET0_WORK_EN
constexpr uint16_t FLAG_UNIT1_WORK = 0x0100;   // CONF.TIMER_UNIT1_WORK_EN

// 128 Samples bei 50 Hz = 2,56 s Vorgeschichte. Der INT_WDT feuert nach 300 ms,
// das Todesfenster passt also mehrfach hinein.
constexpr uint16_t RING_LEN  = 128;
constexpr uint32_t BB_MAGIC  = 0x5B10C0BBu;   // "SixBack blackbox", Version 1

struct Sample {
    uint32_t tick;      // xTaskGetTickCountFromISR()
    uint32_t u0_lo;     // Counter 0 (esptimer) LO — wrappt alle 268,435 s
    uint32_t u1_lo;     // Counter 1 (OS-Tick)  LO
    uint32_t rt0_lo;    // REAL_TARGET0 LO — der tatsaechliche Komparatorwert
    uint16_t u0_hi;     // HI ist 20 bit; die unteren 16 reichen fuer das Fenster
    uint16_t u1_hi;
    uint16_t rt0_hi;
    uint16_t ack0;      // CPU-Zyklen bis VALUE_VALID, 0xFFFF = Timeout
    uint16_t ack1;
    uint16_t flags;
    uint16_t seq;
};

struct Ring {
    uint32_t magic;
    uint16_t widx;        // naechster Schreibindex
    uint16_t wrapped;     // 1 sobald einmal umgelaufen
    uint32_t seq;         // fortlaufend, ueberlebt den Umlauf
    uint32_t isr_count;
    uint32_t ack_max0;
    uint32_t ack_max1;
    uint32_t ack_timeouts0;
    uint32_t ack_timeouts1;
    uint32_t torn0;
    uint32_t torn1;
    // Tick-Freeze direkt im ISR erkannt. Der 13:53-Ring hat am Blech belegt,
    // dass der ISR im Todesfenster weiterlaeuft (14 Samples mit stehendem
    // Tick aufgezeichnet) — damit ist das hier die eindeutige Messung, die
    // der loop()-Detektor prinzipiell nicht leisten kann: dort ist ein echter
    // Tick-Rueckstand von einem kleinen Counter-0-Vorwaertssprung nicht
    // unterscheidbar, beide sehen identisch aus.
    uint32_t freeze_events;
    uint32_t freeze_max_ms;
    // Snapshot-Anforderung: der ISR setzt nur ein Flag und zwei Worte. Die
    // 4-KB-Kopie zieht loop() — der ISR ist das Messinstrument und darf das
    // System, das er vermisst, in der heissen Phase nicht selbst stoeren.
    // Latenzbudget: Ringtiefe 2,56 s gegen loop()-Takt ~55 ms.
    uint32_t req_dur_ms;
    uint32_t req_end_seq;
    uint32_t req_pending;
    uint32_t prev_tick;
    uint16_t frozen_run;      // aufeinanderfolgende Samples mit unveraendertem Tick
    uint16_t _pad;
    uint32_t target0_period;  // TARGET0_CONF.period, einmal beim Init gelesen
    Sample   s[RING_LEN];
};

// RTC-NOINIT: ueberlebt Reset und Panic, wird nur beim Power-On-Reset von der
// Hardware zerstoert. Genau die Eigenschaft, auf der die Blackbox beruht.
RTC_NOINIT_ATTR Ring g_ring;

// Eigener RTC-NOINIT-Puffer mit EIGENER Magic fuer ueberlebte Freeze-Ereignisse.
// Bewusst getrennt vom Panic-Pfad: so kann ein Panic den Freeze-Snapshot nicht
// verdraengen, und der Snapshot ueberlebt einen nachfolgenden Panic-Reboot —
// eine Freeze-danach-Panic-Sequenz liefert damit BEIDE Sichten. Dass solche
// Kopplungen vorkommen, steht im Log: auf den 131-ms-Lag vom 26.08. folgte
// unmittelbar ein Counter-0-Sprung.
constexpr uint32_t SNAP_MAGIC = 0x5B10F2EEu;   // "SixBack freeze", Version 1

struct FreezeSnap {
    uint32_t magic;
    uint32_t dur_ms;       // Laenge des Stillstands (Sample-Zahl x 20 ms)
    uint32_t start_seq;
    uint32_t end_seq;
    uint32_t at_mono_s;    // sprungbereinigte Uptime beim Ereignis
    uint32_t harvested;    // 1 = ueber /api/dbg/blackbox abgeholt
    uint32_t partial;      // 1 = Fenster war beim Kopieren teils ueberschrieben
    Sample   s[RING_LEN];
};
RTC_NOINIT_ATTR FreezeSnap g_snap;

// Eingefrorene Kopie des Vor-Panic-Zustands. Normales RAM, nur zum Ausgeben.
Ring     g_frozen;
bool     g_have_frozen = false;
uint32_t g_frozen_reason = 0;

gptimer_handle_t g_timer = nullptr;
bool             g_running = false;

inline uint32_t rd(uint32_t addr) { return *(volatile uint32_t*)addr; }
inline void     wr(uint32_t addr, uint32_t v) { *(volatile uint32_t*)addr = v; }

// Gedeckelter Counter-Read mit demselben Handshake, den die ROM-Routine
// unbeschraenkt pollt. Gibt die Ack-Latenz in CPU-Zyklen zurueck.
// WICHTIG: liest HI, LO, dann HI erneut — weicht das zweite HI ab, war der
// Read zerrissen. Das ist der direkte Test auf die 268,435-s-Signatur.
IRAM_ATTR bool readCounter(uint32_t opReg, uint32_t hiReg, uint32_t loReg,
                           uint32_t& lo, uint16_t& hi, uint16_t& ack, bool& torn) {
    const uint32_t t0 = esp_cpu_get_cycle_count();
    wr(opReg, BIT_UPDATE);
    while ((rd(opReg) & BIT_VALUE_VALID) == 0) {
        if ((esp_cpu_get_cycle_count() - t0) > ACK_TIMEOUT_CYCLES) {
            ack = ACK_TIMEOUT_MARK; lo = 0; hi = 0; torn = false;
            return false;
        }
    }
    const uint32_t spent = esp_cpu_get_cycle_count() - t0;
    ack = (uint16_t)(spent > ACK_CAP ? ACK_CAP : spent);

    const uint32_t hi1 = rd(hiReg);
    lo  = rd(loReg);
    const uint32_t hi2 = rd(hiReg);
    hi   = (uint16_t)(hi1 & 0xFFFFu);
    torn = (hi1 != hi2);
    return true;
}

IRAM_ATTR bool onTimer(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) {
    Sample s;
    uint16_t flags = 0;
    bool torn0 = false, torn1 = false;

    s.tick = (uint32_t)xTaskGetTickCountFromISR();

    if (!readCounter(REG_UNIT0_OP, REG_UNIT0_VAL_HI, REG_UNIT0_VAL_LO,
                     s.u0_lo, s.u0_hi, s.ack0, torn0)) {
        flags |= FLAG_TIMEOUT_U0; ++g_ring.ack_timeouts0;
    } else if (s.ack0 > g_ring.ack_max0) {
        g_ring.ack_max0 = s.ack0;
    }
    if (torn0) { flags |= FLAG_TORN_U0; ++g_ring.torn0; }

    if (!readCounter(REG_UNIT1_OP, REG_UNIT1_VAL_HI, REG_UNIT1_VAL_LO,
                     s.u1_lo, s.u1_hi, s.ack1, torn1)) {
        flags |= FLAG_TIMEOUT_U1; ++g_ring.ack_timeouts1;
    } else if (s.ack1 > g_ring.ack_max1) {
        g_ring.ack_max1 = s.ack1;
    }
    if (torn1) { flags |= FLAG_TORN_U1; ++g_ring.torn1; }

    // Der tatsaechliche Komparatorwert des OS-Tick-Alarms. Springt Counter 1
    // zurueck, muss dieser Wert weit VOR dem neuen Zaehlerstand liegen — dann
    // feuert kein Tick mehr und der INT_WDT laeuft in den Panic.
    s.rt0_lo = rd(REG_REAL_TGT0_LO);
    s.rt0_hi = (uint16_t)(rd(REG_REAL_TGT0_HI) & 0xFFFFu);

    // Statusbits: bleibt der Alarm scharf, waehrend der Tick steht?
    const uint32_t conf   = rd(REG_CONF);
    const uint32_t tconf  = rd(REG_TARGET0_CONF);
    if (rd(REG_INT_RAW) & 0x1u)  flags |= FLAG_INT_RAW;
    if (rd(REG_INT_ENA) & 0x1u)  flags |= FLAG_INT_ENA;
    if (tconf & (1u << 30))      flags |= FLAG_PERIOD_MD;
    if (conf  & (1u << 24))      flags |= FLAG_TGT0_WORK;
    if (conf  & (1u << 29))      flags |= FLAG_UNIT1_WORK;

    // Freeze-Detektor: Tick unveraendert bei laufendem Counter 0.
    if (s.tick == g_ring.prev_tick) {
        if (g_ring.frozen_run < 0xFFFF) ++g_ring.frozen_run;
        if (g_ring.frozen_run == 3) ++g_ring.freeze_events;   // >= 60 ms, einmal je Episode
        if (g_ring.frozen_run >= 3) {
            const uint32_t ms = (uint32_t)g_ring.frozen_run * 20u;
            if (ms > g_ring.freeze_max_ms) g_ring.freeze_max_ms = ms;
        }
    } else {
        if (g_ring.frozen_run >= 3) {
            // Der Stillstand ist gerade zu Ende — Snapshot anfordern. Nur Flag
            // und zwei Worte, kein memcpy im ISR.
            g_ring.req_dur_ms  = (uint32_t)g_ring.frozen_run * 20u;
            g_ring.req_end_seq = g_ring.seq;
            g_ring.req_pending = 1;
        }
        g_ring.frozen_run = 0;
    }
    g_ring.prev_tick = s.tick;

    s.flags = flags;
    s.seq   = (uint16_t)(g_ring.seq & 0xFFFFu);

    g_ring.s[g_ring.widx] = s;
    g_ring.widx = (uint16_t)((g_ring.widx + 1) % RING_LEN);
    if (g_ring.widx == 0) g_ring.wrapped = 1;
    ++g_ring.seq;
    ++g_ring.isr_count;
    return false;   // kein Task-Wakeup noetig
}

void ringReset() {
    g_ring.magic = BB_MAGIC;
    g_ring.widx = 0; g_ring.wrapped = 0; g_ring.seq = 0; g_ring.isr_count = 0;
    g_ring.ack_max0 = 0; g_ring.ack_max1 = 0;
    g_ring.ack_timeouts0 = 0; g_ring.ack_timeouts1 = 0;
    g_ring.torn0 = 0; g_ring.torn1 = 0;
    g_ring.freeze_events = 0; g_ring.freeze_max_ms = 0;
    g_ring.prev_tick = 0; g_ring.frozen_run = 0;
    g_ring.req_dur_ms = 0; g_ring.req_end_seq = 0; g_ring.req_pending = 0;
    g_ring.target0_period = 0;
    for (uint16_t i = 0; i < RING_LEN; ++i) g_ring.s[i] = Sample{};
}

// Eine Sample-Zeile ins JSON. EINE Stelle fuer die Spaltenreihenfolge, damit
// Live-, Freeze- und Panic-Ausgabe nie auseinanderlaufen.
void sampleToArray(const Sample& s, JsonArray r) {
    r.add(s.seq);      //  0 laufende Nummer
    r.add(s.tick);     //  1 OS-Tick-Zaehler
    r.add(s.u1_hi);    //  2 Counter 1 (OS-Tick) HI
    r.add(s.u1_lo);    //  3 Counter 1 LO
    r.add(s.rt0_hi);   //  4 REAL_TARGET0 HI  (tatsaechlicher Komparator)
    r.add(s.rt0_lo);   //  5 REAL_TARGET0 LO
    r.add(s.u0_hi);    //  6 Counter 0 (esptimer) HI
    r.add(s.u0_lo);    //  7 Counter 0 LO
    r.add(s.ack1);     //  8 Ack-Zyklen Counter 1
    r.add(s.ack0);     //  9 Ack-Zyklen Counter 0
    r.add(s.flags);    // 10 Bit0/1 TO-u0/u1, Bit2/3 torn-u0/u1, Bit4 INT_RAW,
                       //    Bit5 INT_ENA, Bit6 period_mode, Bit7 TGT0_WORK, Bit8 UNIT1_WORK
}

// Ring aelteste->juengste in ein JSON-Array, damit der Verlauf lesbar ist.
void ringToArray(const Ring& r, JsonArray arr) {
    const uint16_t n     = r.wrapped ? RING_LEN : r.widx;
    const uint16_t start = r.wrapped ? r.widx : 0;
    for (uint16_t i = 0; i < n; ++i) sampleToArray(r.s[(start + i) % RING_LEN], arr.add<JsonArray>());
}

// Sperre gegen ein Ueberschreiben des Freeze-Snapshots waehrend ein Leser ihn
// gerade herausstreamt. Der Leser laeuft im async_tcp-Task, das Ueberschreiben
// in loop() — ohne die Sperre bekaeme ein langsamer Client eine Zeile aus dem
// alten und die naechste aus dem neuen Snapshot, ohne dass es auffiele.
// BEWUSST normales RAM: die RTC-Structs duerfen ihr Layout nicht aendern, sonst
// wird ein aus dem Vorboot ueberlebender Snapshot falsch gelesen.
volatile bool     g_dumpBusy  = false;
volatile uint32_t g_busyTicks = 0;   // loop()-Durchlaeufe mit gehaltener Sperre

// Notbremse: ein abgerissener Client, dessen Response nie zerstoert wird,
// duerfte die Sperre nicht auf Dauer halten — sonst faengt das Board nie wieder
// einen Snapshot. Gezaehlt wird in loop()-Durchlaeufen und nicht in
// Millisekunden, weil genau die Zeitbasis hier der Untersuchungsgegenstand ist.
constexpr uint32_t BUSY_TICK_LIMIT = 3000;   // ~3 min bei ~55 ms loop()

}  // namespace

void blackboxFreezeService() {
    if (!g_ring.req_pending) return;

    // Liest gerade jemand? Dann Anforderung STEHEN LASSEN und spaeter kopieren.
    if (g_dumpBusy) {
        if (++g_busyTicks < BUSY_TICK_LIMIT) return;
        Serial.println("[blackbox] Dump-Sperre haengt — wird zwangsweise gelöst.");
        g_dumpBusy = false; g_busyTicks = 0;
    }

    const uint32_t dur    = g_ring.req_dur_ms;
    const uint32_t endseq = g_ring.req_end_seq;
    g_ring.req_pending = 0;

    // Retention: ueberschreiben nur, wenn der vorhandene Snapshot abgeholt ist
    // ODER der neue laenger war. So geht der laengste, todesnaechste ueberlebte
    // Fall nie verloren, und ein abgeholter blockiert nichts.
    if (g_snap.magic == SNAP_MAGIC && !g_snap.harvested && dur <= g_snap.dur_ms) return;

    const uint32_t seq_before = g_ring.seq;
    for (uint16_t i = 0; i < RING_LEN; ++i) g_snap.s[i] = g_ring.s[i];
    const uint32_t seq_after  = g_ring.seq;

    g_snap.magic     = SNAP_MAGIC;
    g_snap.dur_ms    = dur;
    g_snap.end_seq   = endseq;
    g_snap.start_seq = endseq - (dur / 20u);
    g_snap.at_mono_s = monoUptimeS();
    g_snap.harvested = 0;
    // Lag das Freeze-Fenster beim Kopieren noch im Ring? Nur bei >2 s
    // loop()-Blockade kann es herausgelaufen sein — dann als partial melden
    // statt verwerfen.
    g_snap.partial   = ((seq_after - endseq) > (uint32_t)(RING_LEN - 4)) ? 1u : 0u;

    Serial.printf("[blackbox] Freeze-Snapshot gesichert: %lu ms, seq %lu..%lu, mono=%lus%s "
                  "(Ring-seq %lu->%lu)\n",
                  (unsigned long)g_snap.dur_ms, (unsigned long)g_snap.start_seq,
                  (unsigned long)g_snap.end_seq, (unsigned long)g_snap.at_mono_s,
                  g_snap.partial ? " PARTIAL" : "",
                  (unsigned long)seq_before, (unsigned long)seq_after);
}

void blackboxInit() {
    const esp_reset_reason_t rr = esp_reset_reason();
    // Nur ein unsauberer Reset kann einen auswertbaren Vor-Zustand hinterlassen.
    // Nach POWERON ist der RTC-Inhalt Muell, nach einem gewollten SW-Reset
    // (OTA, /api/reboot) ist er uninteressant.
    const bool interesting = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                              rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                              rr == ESP_RST_BROWNOUT);

    if (g_ring.magic == BB_MAGIC && interesting) {
        g_frozen        = g_ring;      // Kopie ziehen, BEVOR wir neu anfangen
        g_have_frozen   = true;
        g_frozen_reason = (uint32_t)rr;
        Serial.printf("[blackbox] Vor-Panic-Ring gesichert: %u Samples, seq=%lu, "
                      "ack_max u0=%lu u1=%lu, timeouts u0=%lu u1=%lu, torn u0=%lu u1=%lu\n",
                      (unsigned)(g_frozen.wrapped ? RING_LEN : g_frozen.widx),
                      (unsigned long)g_frozen.seq,
                      (unsigned long)g_frozen.ack_max0, (unsigned long)g_frozen.ack_max1,
                      (unsigned long)g_frozen.ack_timeouts0, (unsigned long)g_frozen.ack_timeouts1,
                      (unsigned long)g_frozen.torn0, (unsigned long)g_frozen.torn1);
    } else if (g_ring.magic == BB_MAGIC) {
        Serial.println("[blackbox] Ring vorhanden, aber sauberer Reset — verworfen.");
    }
    ringReset();

    // Freeze-Snapshot: nur nach POWERON ist der RTC-Inhalt Muell. Bei jedem
    // anderen Reset bleibt ein vorhandener Snapshot stehen — er soll einen
    // Panic ausdruecklich ueberleben.
    if (rr == ESP_RST_POWERON || g_snap.magic != SNAP_MAGIC) {
        g_snap.magic = 0; g_snap.dur_ms = 0; g_snap.harvested = 0; g_snap.partial = 0;
    } else {
        Serial.printf("[blackbox] Freeze-Snapshot aus vorherigem Boot vorhanden: %lu ms, "
                      "mono=%lus, harvested=%lu\n",
                      (unsigned long)g_snap.dur_ms, (unsigned long)g_snap.at_mono_s,
                      (unsigned long)g_snap.harvested);
    }

    // GPTimer: eigene TIMG-Peripherie, NICHT der SYSTIMER. Genau deshalb kann
    // sie messen, waehrend der SYSTIMER klemmt. ISR in IRAM, damit sie auch
    // ohne Flash-Cache laeuft; hoechste Prioritaet, damit sie eine haengende
    // Tick-ISR ueberhaupt praeemptieren kann.
    // VORBEHALT: ob sie das im echten Todesfenster schafft, ist NICHT belegt —
    // bei der Idle-WFI-Signatur sicher (jeder Interrupt weckt den Kern), bei
    // der ROM-Poll-Signatur spricht dafuer, dass der INT_WDT die Tick-ISR
    // nachweislich praeemptieren konnte. Am Blech zu verifizieren.
    gptimer_config_t cfg = {};
    cfg.clk_src       = GPTIMER_CLK_SRC_DEFAULT;
    cfg.direction     = GPTIMER_COUNT_UP;
    cfg.resolution_hz = 1000000;        // 1 us
    cfg.intr_priority = 3;
    cfg.flags.intr_shared = 0;

    if (gptimer_new_timer(&cfg, &g_timer) != ESP_OK) {
        Serial.println("[blackbox] gptimer_new_timer fehlgeschlagen — Blackbox AUS.");
        g_timer = nullptr;
        return;
    }
    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = onTimer;
    if (gptimer_register_event_callbacks(g_timer, &cbs, nullptr) != ESP_OK) {
        Serial.println("[blackbox] Callback-Registrierung fehlgeschlagen — Blackbox AUS.");
        gptimer_del_timer(g_timer); g_timer = nullptr;
        return;
    }
    gptimer_alarm_config_t al = {};
    al.alarm_count = 20000;             // 20 ms -> 50 Hz
    al.reload_count = 0;
    al.flags.auto_reload_on_alarm = true;
    gptimer_set_alarm_action(g_timer, &al);
    gptimer_enable(g_timer);
    gptimer_start(g_timer);
    g_running = true;
    // Erwartung 16000 = 1000 us Tickperiode x 16 Ticks/us. Weicht der Wert ab,
    // stimmt die Annahme ueber den Tick-Alarm nicht und alles Weitere kippt.
    g_ring.target0_period = rd(REG_TARGET0_CONF) & 0x03FFFFFFu;
    Serial.printf("[blackbox] laeuft — GPTimer 50 Hz, IRAM-ISR, RTC-NOINIT-Ring %u Samples, "
                  "TARGET0_CONF.period=%lu (erwartet 16000)\n",
                  (unsigned)RING_LEN, (unsigned long)g_ring.target0_period);
}

void blackboxToJson(JsonObject health) {
    JsonObject bb = health["blackbox"].to<JsonObject>();
    bb["running"]   = g_running;
    bb["hz"]        = 50;
    bb["ring_len"]  = RING_LEN;
    bb["isr_count"] = g_ring.isr_count;

    // Laufende Handshake-Statistik dieses Boots. ack_* in CPU-Zyklen bei
    // 160 MHz. Auf gesunder Hardware sind das einstellige bis niedrig
    // zweistellige Werte; Timeouts und torn-Zaehler muessen 0 bleiben.
    JsonObject live = bb["live"].to<JsonObject>();
    live["ack_max_cyc_esptimer"] = g_ring.ack_max0;
    live["ack_max_cyc_ostick"]   = g_ring.ack_max1;
    live["ack_timeouts_esptimer"] = g_ring.ack_timeouts0;
    live["ack_timeouts_ostick"]   = g_ring.ack_timeouts1;
    live["torn_reads_esptimer"]   = g_ring.torn0;
    live["torn_reads_ostick"]     = g_ring.torn1;
    live["tick_freeze_events"]    = g_ring.freeze_events;
    live["tick_freeze_max_ms"]    = g_ring.freeze_max_ms;
    live["target0_period"]        = g_ring.target0_period;

    // Freeze-Snapshot: in /api/status NUR Skalare — die Sample-Zeilen holt
    // /api/dbg/blackbox, sonst blaeht das Status-JSON um mehrere KB auf.
    JsonObject fsn = bb["freeze_snap"].to<JsonObject>();
    const bool have_snap = (g_snap.magic == SNAP_MAGIC);
    fsn["present"] = have_snap;
    if (have_snap) {
        fsn["dur_ms"]    = g_snap.dur_ms;
        fsn["at_mono_s"] = g_snap.at_mono_s;
        fsn["seq_start"] = g_snap.start_seq;
        fsn["seq_end"]   = g_snap.end_seq;
        fsn["harvested"] = (bool)g_snap.harvested;
        fsn["partial"]   = (bool)g_snap.partial;
    }

    if (!g_have_frozen) { bb["frozen"] = false; return; }

    // Vor-Panic-Zustand. Die Samples stehen in Schreibreihenfolge; das juengste
    // liegt bei widx-1. Ausgegeben wird aelteste -> juengste, damit der Verlauf
    // in den Todesmoment hinein lesbar ist.
    bb["frozen"] = true;
    bb["frozen_reset_reason"] = g_frozen_reason;
    JsonObject fs = bb["frozen_stats"].to<JsonObject>();
    fs["seq"]                    = g_frozen.seq;
    fs["isr_count"]              = g_frozen.isr_count;
    fs["ack_max_cyc_esptimer"]   = g_frozen.ack_max0;
    fs["ack_max_cyc_ostick"]     = g_frozen.ack_max1;
    fs["ack_timeouts_esptimer"]  = g_frozen.ack_timeouts0;
    fs["ack_timeouts_ostick"]    = g_frozen.ack_timeouts1;
    fs["torn_reads_esptimer"]    = g_frozen.torn0;
    fs["torn_reads_ostick"]      = g_frozen.torn1;
    fs["tick_freeze_events"]     = g_frozen.freeze_events;
    fs["tick_freeze_max_ms"]     = g_frozen.freeze_max_ms;
    fs["target0_period"]         = g_frozen.target0_period;

    JsonArray arr = bb["frozen_samples"].to<JsonArray>();
    ringToArray(g_frozen, arr);
}

// ---------------------------------------------------------------------------
// GET /api/dbg/blackbox — gechunkte Ausgabe.
//
// WARUM NICHT MEHR ALS JsonDocument: die Vorgaengerfassung baute alle drei
// Ringe (Live + Freeze-Snapshot + Vor-Panic, zusammen 384 Sample-Zeilen) in ein
// JsonDocument und serialisierte das in einen AsyncResponseStream. Beides liegt
// gleichzeitig im Heap — grob 40-50 KB Dokument plus die mitwachsende
// Stream-Kopie — und das reisst auf dem C6 bei min_free ~110 KB die
// Allokation ab; der async_tcp-Task dreht dann durch, bis der TWDT das Board
// resettet (am Blech beobachtet 29.08.2026). Der Endpoint war damit genau dann
// unbrauchbar, wenn er gebraucht wurde: ein ueberlebter Freeze-Snapshot war nur
// um den Preis eines Reboots lesbar, der die laufende Messung beendet.
//
// Diese Fassung formatiert je Aufruf ein paar Zeilen direkt aus dem RTC-Ring in
// den Chunk-Puffer des Servers. Kein JsonDocument, keine Zwischenkopie, keine
// Allokation im Sendepfad; der Speicherbedarf ist konstant und unabhaengig von
// der Ringlaenge.
//
// Auswahl per Query: ?ring=live|snap|frozen (Default alle drei),
// ?from=<n>&count=<m> als Fenster in den ausgewaehlten Ring.
// ---------------------------------------------------------------------------
namespace {

enum : uint8_t {
    PH_HEAD = 0, PH_LIVE_STATS, PH_LIVE_OPEN, PH_LIVE_ROWS, PH_LIVE_CLOSE,
    PH_SNAP_OPEN, PH_SNAP_ROWS, PH_SNAP_CLOSE,
    PH_FROZ_OPEN, PH_FROZ_ROWS, PH_FROZ_CLOSE,
    PH_TAIL, PH_DONE,
};

// Groesste Einheit, die am Stueck geschrieben wird. Eine Sample-Zeile ist
// hoechstens ~80 Zeichen, die Kopfbloecke bleiben bewusst darunter.
constexpr size_t PUT_MAX = 300;

// Haengt an, wenn es VOLLSTAENDIG passt; sonst false, dann kommt der Rest in
// den naechsten Chunk. Nie eine halbe Zeile schreiben — der Leser bekaeme
// sonst kaputtes JSON, ohne dass ein Fehler sichtbar wuerde.
bool put(uint8_t* buf, size_t maxLen, size_t& used, const char* fmt, ...) {
    char tmp[PUT_MAX];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n <= 0) return true;                       // nichts zu schreiben
    if ((size_t)n >= maxLen - used) return false;  // passt nicht mehr
    memcpy(buf + used, tmp, (size_t)n);
    used += (size_t)n;
    return true;
}

// EINE Sample-Zeile. Spaltenreihenfolge identisch zu sampleToArray() —
// wer hier etwas verschiebt, muss es dort mitziehen. Der Endpoint gibt die
// Reihenfolge zusaetzlich als "cols" mit aus, damit ein Auswerter sie nicht
// aus dem Quelltext rekonstruieren muss (genau daran ist am 30.08.2026 eine
// Auswertung fast gescheitert).
bool putSample(uint8_t* buf, size_t maxLen, size_t& used, const Sample& s, bool comma) {
    return put(buf, maxLen, used, "%s[%u,%lu,%u,%lu,%u,%lu,%u,%lu,%u,%u,%u]",
               comma ? "," : "",
               (unsigned)s.seq,     (unsigned long)s.tick,
               (unsigned)s.u1_hi,   (unsigned long)s.u1_lo,
               (unsigned)s.rt0_hi,  (unsigned long)s.rt0_lo,
               (unsigned)s.u0_hi,   (unsigned long)s.u0_lo,
               (unsigned)s.ack1,    (unsigned)s.ack0, (unsigned)s.flags);
}

// Sample i eines Rings in Lesereihenfolge (aeltestes zuerst).
inline const Sample& ringAt(const Ring& r, uint16_t i) {
    const uint16_t start = r.wrapped ? r.widx : 0;
    return r.s[(uint16_t)((start + i) % RING_LEN)];
}
inline uint16_t ringCount(const Ring& r) { return r.wrapped ? RING_LEN : r.widx; }

// Fensterung: liefert die Zahl auszugebender Zeilen und setzt idx auf den Start.
uint16_t windowInto(uint16_t total, uint16_t from, uint16_t count, uint16_t& idx) {
    if (from >= total) { idx = total; return 0; }
    idx = from;
    const uint16_t rest = (uint16_t)(total - from);
    return (count < rest) ? count : rest;
}

}  // namespace

BlackboxDump::BlackboxDump(uint8_t sel_, uint16_t from_, uint16_t count_)
    : sel(sel_), phase(PH_HEAD), idx(0), n(0), from(from_), count(count_), locked(false) {
    // Sperre nur nehmen, wenn der Snapshot ueberhaupt Teil der Ausgabe ist.
    if (sel & BB_SEL_SNAP) { g_dumpBusy = true; g_busyTicks = 0; locked = true; }
}

BlackboxDump::~BlackboxDump() {
    if (locked) { g_dumpBusy = false; g_busyTicks = 0; }
}

bool blackboxAckSnapshot() {
    if (g_snap.magic != SNAP_MAGIC) return false;
    g_snap.harvested = 1;
    Serial.printf("[blackbox] Freeze-Snapshot quittiert (%lu ms, mono=%lus) — "
                  "Platz fuer den naechsten frei.\n",
                  (unsigned long)g_snap.dur_ms, (unsigned long)g_snap.at_mono_s);
    return true;
}

size_t blackboxDumpFill(BlackboxDump& st, uint8_t* buf, size_t maxLen, bool& more) {
    size_t used = 0;
    more = false;
    if (maxLen <= PUT_MAX) { more = (st.phase != PH_DONE); return 0; }

    const bool have_snap = (g_snap.magic == SNAP_MAGIC);

    for (;;) {
        switch (st.phase) {

        case PH_HEAD:
            if (!put(buf, maxLen, used,
                     "{\"cols\":\"seq,os_tick,c1_hi,c1_lo,rt0_hi,rt0_lo,c0_hi,c0_lo,"
                     "ack_c1,ack_c0,flags\",\"flag_bits\":\"0x01/0x02=ack_timeout_c0/c1,"
                     "0x04/0x08=torn_c0/c1,0x10=INT_RAW,0x20=INT_ENA,0x40=period_mode,"
                     "0x80=TGT0_WORK,0x100=UNIT1_WORK\","))
                break;
            st.phase = PH_LIVE_STATS;
            continue;

        case PH_LIVE_STATS:
            if (!put(buf, maxLen, used,
                     "\"running\":%s,\"hz\":50,\"ring_len\":%u,\"isr_count\":%lu,"
                     "\"sel\":%u,\"live\":{\"seq\":%lu,\"ack_max_c0\":%lu,\"ack_max_c1\":%lu,"
                     "\"ack_timeouts_c0\":%lu,\"ack_timeouts_c1\":%lu,\"torn_c0\":%lu,"
                     "\"torn_c1\":%lu,\"tick_freeze_events\":%lu,\"tick_freeze_max_ms\":%lu,"
                     "\"target0_period\":%lu}",
                     g_running ? "true" : "false", (unsigned)RING_LEN,
                     (unsigned long)g_ring.isr_count, (unsigned)st.sel,
                     (unsigned long)g_ring.seq,
                     (unsigned long)g_ring.ack_max0, (unsigned long)g_ring.ack_max1,
                     (unsigned long)g_ring.ack_timeouts0, (unsigned long)g_ring.ack_timeouts1,
                     (unsigned long)g_ring.torn0, (unsigned long)g_ring.torn1,
                     (unsigned long)g_ring.freeze_events, (unsigned long)g_ring.freeze_max_ms,
                     (unsigned long)g_ring.target0_period))
                break;
            st.phase = PH_LIVE_OPEN;
            continue;

        case PH_LIVE_OPEN:
            if (!(st.sel & BB_SEL_LIVE)) { st.phase = PH_SNAP_OPEN; continue; }
            // Der Live-Ring wird waehrend des Lesens vom ISR weiterbeschrieben.
            // Das ist gewollt (er zeigt den Jetzt-Zustand), macht die aeltesten
            // Zeilen aber potenziell inkonsistent — deshalb ausgewiesen.
            if (!put(buf, maxLen, used,
                     ",\"live_samples\":{\"order\":\"chronological\",\"racy\":true,"
                     "\"n\":%u,\"from\":%u,\"rows\":[",
                     (unsigned)(st.n = windowInto(ringCount(g_ring), st.from, st.count, st.idx)),
                     (unsigned)st.idx))
                break;
            st.phase = PH_LIVE_ROWS;
            continue;

        case PH_LIVE_ROWS: {
            // idx == from ist genau die erste Zeile des Arrays — nur die
            // bekommt kein fuehrendes Komma. Gilt auch ueber Chunk-Grenzen.
            while (st.n > 0) {
                if (!putSample(buf, maxLen, used, ringAt(g_ring, st.idx), st.idx != st.from)) break;
                ++st.idx; --st.n;
            }
            if (st.n > 0) break;                 // Rest im naechsten Chunk
            st.phase = PH_LIVE_CLOSE;
            continue;
        }

        case PH_LIVE_CLOSE:
            if (!put(buf, maxLen, used, "]}")) break;
            st.phase = PH_SNAP_OPEN;
            continue;

        case PH_SNAP_OPEN:
            if (!(st.sel & BB_SEL_SNAP)) { st.phase = PH_FROZ_OPEN; continue; }
            if (!have_snap) {
                if (!put(buf, maxLen, used, ",\"freeze_snap\":{\"present\":false}")) break;
                st.phase = PH_FROZ_OPEN;
                continue;
            }
            // Der Snapshot ist eine ROHE Slot-Kopie des Rings — die Zeilen
            // stehen NICHT in zeitlicher Reihenfolge. Der Leser sortiert nach
            // seq (16 Bit, kann umlaufen). Bewusst so gelassen: eine
            // Umsortierung braeuchte den Schreibindex im Snapshot, und das
            // waere eine Layout-Aenderung an einer RTC-Struktur, die einen aus
            // dem Vorboot ueberlebenden Snapshot unlesbar machen wuerde.
            if (!put(buf, maxLen, used,
                     ",\"freeze_snap\":{\"present\":true,\"order\":\"slot\","
                     "\"sort_by\":\"seq\",\"dur_ms\":%lu,\"at_mono_s\":%lu,"
                     "\"seq_start\":%lu,\"seq_end\":%lu,\"partial\":%s,"
                     "\"harvested\":%s,\"n\":%u,\"from\":%u,\"rows\":[",
                     (unsigned long)g_snap.dur_ms, (unsigned long)g_snap.at_mono_s,
                     (unsigned long)g_snap.start_seq, (unsigned long)g_snap.end_seq,
                     g_snap.partial ? "true" : "false",
                     g_snap.harvested ? "true" : "false",
                     (unsigned)(st.n = windowInto(RING_LEN, st.from, st.count, st.idx)),
                     (unsigned)st.idx))
                break;
            st.phase = PH_SNAP_ROWS;
            continue;

        case PH_SNAP_ROWS: {
            // idx == from ist genau die erste Zeile des Arrays — nur die
            // bekommt kein fuehrendes Komma. Gilt auch ueber Chunk-Grenzen.
            while (st.n > 0) {
                if (!putSample(buf, maxLen, used, g_snap.s[st.idx], st.idx != st.from)) break;
                ++st.idx; --st.n;
            }
            if (st.n > 0) break;                 // Rest im naechsten Chunk
            st.phase = PH_SNAP_CLOSE;
            continue;
        }

        case PH_SNAP_CLOSE:
            if (!put(buf, maxLen, used, "]}")) break;
            st.phase = PH_FROZ_OPEN;
            continue;

        case PH_FROZ_OPEN:
            if (!(st.sel & BB_SEL_FROZEN)) { st.phase = PH_TAIL; continue; }
            if (!g_have_frozen) {
                if (!put(buf, maxLen, used, ",\"frozen\":{\"present\":false}")) break;
                st.phase = PH_TAIL;
                continue;
            }
            if (!put(buf, maxLen, used,
                     ",\"frozen\":{\"present\":true,\"order\":\"chronological\","
                     "\"reset_reason\":%lu,\"seq\":%lu,\"ack_max_c0\":%lu,\"ack_max_c1\":%lu,"
                     "\"ack_timeouts_c0\":%lu,\"ack_timeouts_c1\":%lu,\"torn_c0\":%lu,"
                     "\"torn_c1\":%lu,\"tick_freeze_events\":%lu,\"tick_freeze_max_ms\":%lu,"
                     "\"n\":%u,\"from\":%u,\"rows\":[",
                     (unsigned long)g_frozen_reason, (unsigned long)g_frozen.seq,
                     (unsigned long)g_frozen.ack_max0, (unsigned long)g_frozen.ack_max1,
                     (unsigned long)g_frozen.ack_timeouts0, (unsigned long)g_frozen.ack_timeouts1,
                     (unsigned long)g_frozen.torn0, (unsigned long)g_frozen.torn1,
                     (unsigned long)g_frozen.freeze_events, (unsigned long)g_frozen.freeze_max_ms,
                     (unsigned)(st.n = windowInto(ringCount(g_frozen), st.from, st.count, st.idx)),
                     (unsigned)st.idx))
                break;
            st.phase = PH_FROZ_ROWS;
            continue;

        case PH_FROZ_ROWS: {
            // idx == from ist genau die erste Zeile des Arrays — nur die
            // bekommt kein fuehrendes Komma. Gilt auch ueber Chunk-Grenzen.
            while (st.n > 0) {
                if (!putSample(buf, maxLen, used, ringAt(g_frozen, st.idx), st.idx != st.from)) break;
                ++st.idx; --st.n;
            }
            if (st.n > 0) break;                 // Rest im naechsten Chunk
            st.phase = PH_FROZ_CLOSE;
            continue;
        }

        case PH_FROZ_CLOSE:
            if (!put(buf, maxLen, used, "]}")) break;
            st.phase = PH_TAIL;
            continue;

        case PH_TAIL:
            if (!put(buf, maxLen, used, "}")) break;
            st.phase = PH_DONE;
            continue;

        case PH_DONE:
        default:
            return used;   // erst der Folgeaufruf liefert 0 und beendet den Stream
        }
        break;   // aus dem switch gefallen = Puffer voll
    }

    if (used == 0) more = true;   // nichts untergebracht, aber noch nicht fertig
    return used;
}

}  // namespace sixback

#else   // ohne Build-Flag: No-Op, damit die Aufrufer target-unabhaengig bleiben

namespace sixback {
void blackboxInit() {}
void blackboxToJson(JsonObject) {}
void blackboxFreezeService() {}
BlackboxDump::BlackboxDump(uint8_t sel_, uint16_t from_, uint16_t count_)
    : sel(sel_), phase(0), idx(0), n(0), from(from_), count(count_), locked(false) {}
BlackboxDump::~BlackboxDump() {}
bool blackboxAckSnapshot() { return false; }
size_t blackboxDumpFill(BlackboxDump&, uint8_t* buf, size_t maxLen, bool& more) {
    more = false;
    const char* empty = "{\"running\":false}";
    const size_t n = strlen(empty);
    if (maxLen < n) return 0;
    memcpy(buf, empty, n);
    return n;   // Folgeaufruf liefert 0 und beendet den Stream
}
}  // namespace sixback

#endif  // SIXBACK_SYSTIMER_BLACKBOX

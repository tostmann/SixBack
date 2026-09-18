// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — SYSTIMER-Blackbox (nur ESP32-C6, Build-Flag SIXBACK_SYSTIMER_BLACKBOX)
//
// WOZU: Die INT_WDT-Panics auf dem C6-Testboard haengen an der Zeitbasis. Der
// INT_WDT wird ausschliesslich vom OS-Tick gefuettert (int_wdt.c registriert
// wdt_hal_feed() als Tick-Hook), also heisst jeder Panic woertlich "SYSTIMER-
// Counter 1 hat 300 ms nicht geliefert". Die vorhandene Probe in
// system_health.cpp laeuft in loop() und kann genau den Zustand nicht sehen,
// der zum Panic fuehrt: steht der Tick, wird loop() nicht mehr geweckt.
//
// Diese Blackbox schreibt aus einem EIGENEN GPTimer-ISR (TIMG-Peripherie, also
// unabhaengig vom SYSTIMER) mit ~50 Hz in einen Ringpuffer im RTC-NOINIT-RAM.
// Der Puffer ueberlebt den Panic-Reboot; beim naechsten Boot wird er
// eingefroren und ueber /api/status ausgegeben. Kein Panic-Handler-Hook noetig:
// geschrieben wird VOR dem Tod, gelesen danach.
//
// Zwei Fragen soll sie beantworten:
//   (a) Sind die beobachteten Rueckwaertsspruenge korrupte Snapshot-Reads oder
//       springt der Zaehler wirklich? -> rohe LO/HI-Werte beider Counter.
//       SOC_SYSTIMER_BIT_WIDTH_LO ist 32 und der Zaehler laeuft mit 16 MHz
//       (XTAL 40 MHz / fixem Divider 2,5), also wrappt LO alle 268,435 s —
//       exakt die Sprunghoehe, die am Blech gemessen wurde. Ein zerrissener
//       Read mit veraltetem HI erzeugt genau diesen Wert; ein echter Sprung
//       muesste HI mitnehmen. Die Blackbox loggt beides.
//   (b) Was tut Counter 1 im Todesfenster — steht er, oder bleibt nur der
//       Alarm aus? -> Counter-1-Wert gegen Alarm-Target 0 (OS-Tick-Alarm).
//
// Nebenprodukt: Der Counter-Read benutzt denselben VALUE_VALID-Handshake, der
// in der ROM-Routine unbeschraenkt gepollt wird. Hier ist er per CPU-Zyklen-
// zaehler gedeckelt (esp_cpu_get_cycle_count() ist systimer-unabhaengig), und
// die gemessene Ack-Latenz wird fuer BEIDE Counter statistisch gefuehrt. Damit
// wird der Read selbst zur Messung des Handshakes.
//
// Die Blackbox misst nur und greift nirgends ein.

#ifndef BOSEFIX32_SYSTIMER_BLACKBOX_H
#define BOSEFIX32_SYSTIMER_BLACKBOX_H

#include <ArduinoJson.h>
#include <stdint.h>

namespace sixback {

// In setup() aufrufen, nach healthInit() (braucht die Reset-Reason).
// Ohne Build-Flag ein No-Op.
void blackboxInit();

// Fuegt das "blackbox"-Objekt in den health-Block von /api/status ein.
// Ohne Build-Flag wird nichts eingefuegt. Enthaelt nur Skalare — die
// Sample-Zeilen liefert blackboxDbgToJson().
void blackboxToJson(JsonObject health);

// In loop() (aus healthTick()) aufrufen. Zieht die 4-KB-Kopie des Rings,
// wenn der ISR nach einem ueberlebten Tick-Stillstand einen Snapshot
// angefordert hat. Ohne Build-Flag ein No-Op.
void blackboxFreezeService();

// --- Gechunkte Ausgabe fuer GET /api/dbg/blackbox --------------------------
// Der frueher hier stehende Weg (ein JsonDocument mit allen drei Ringen,
// serialisiert in einen AsyncResponseStream) hat auf dem C6 zuverlaessig das
// Board rebootet und war damit genau dann unbrauchbar, wenn er gebraucht wurde:
// ein ueberlebter Freeze-Snapshot war nur um den Preis eines Reboots lesbar,
// der die laufende Messung beendet. Details in systimer_blackbox.cpp.
//
// Stattdessen wird direkt aus dem RTC-Ring in den Chunk-Puffer formatiert:
// keine Zwischenkopie, kein JsonDocument, konstanter Speicherbedarf.

enum : uint8_t {
    BB_SEL_LIVE   = 1,   // laufender Ring (wird waehrend des Lesens beschrieben)
    BB_SEL_SNAP   = 2,   // ueberlebter Freeze-Snapshot
    BB_SEL_FROZEN = 4,   // Vor-Panic-Ring des letzten unsauberen Resets
    BB_SEL_ALL    = 7,
};

// Lesezustand EINER Anfrage. Haelt keine Kopie der Daten, nur Indizes.
// Der Konstruktor nimmt die Sperre gegen ein Ueberschreiben des Snapshots,
// der Destruktor gibt sie zurueck — deshalb gehoert das Objekt in einen
// shared_ptr, den die Response-Lambda am Leben haelt.
struct BlackboxDump {
    uint8_t  sel;
    uint8_t  phase;
    uint16_t idx;      // naechstes Sample im laufenden Abschnitt
    uint16_t n;        // Sample-Zahl des laufenden Abschnitts
    uint16_t from;     // Fenster in den ausgewaehlten Ring
    uint16_t count;
    bool     locked;

    BlackboxDump(uint8_t sel_, uint16_t from_, uint16_t count_);
    ~BlackboxDump();
    BlackboxDump(const BlackboxDump&)            = delete;
    BlackboxDump& operator=(const BlackboxDump&) = delete;
};

// Fuellt den naechsten Chunk. Rueckgabe = geschriebene Bytes.
// 0 mit more==false heisst fertig; 0 mit more==true heisst "Puffer war zu
// klein fuer die naechste Einheit, gleich nochmal fragen" (der Aufrufer setzt
// das in RESPONSE_TRY_AGAIN um — diese Datei kennt den Webserver nicht).
size_t blackboxDumpFill(BlackboxDump& st, uint8_t* buf, size_t maxLen, bool& more);

// Markiert den Freeze-Snapshot als abgeholt und gibt den Platz fuer den
// naechsten frei. BEWUSST explizit statt als Nebenwirkung des Lesens: die alte
// Fassung setzte das Flag beim Serialisieren, also auch dann, wenn die Antwort
// den Client nie erreichte — ein abgebrochener Abruf hat den Snapshot damit
// verbraucht. Gibt false zurueck, wenn keiner vorliegt.
bool blackboxAckSnapshot();

}  // namespace sixback

#endif  // BOSEFIX32_SYSTIMER_BLACKBOX_H

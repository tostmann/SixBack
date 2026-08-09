// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SixBack — NVS-Helper (JSON-Persistenz)
#ifndef SIXBACK_NVS_HELPER_H
#define SIXBACK_NVS_HELPER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

namespace sixback {

// Liest ein NVS-key und parsed JSON in 'doc'. Erkennt drei Generationen
// transparent: Legacy-STRING (<= v0.8.14), Klartext-JSON-Blob (v0.8.15/16)
// und heatshrink-komprimiertes Blob ("HS"-Frame, seit der Kompressions-
// Stufe). Aeltere Formate werden beim naechsten Save in-place ersetzt.
// Gibt false zurueck wenn nicht da, Decode- oder Parse-Fehler.
// dataPresent (optional, FHEM 144729 #153): wird true gesetzt, wenn unter
// ns/key DATEN LIEGEN (Blob/String nicht leer) — erlaubt dem Caller,
// "fresh/leer" (false+false) von "vorhanden aber UNLESBAR" (false+true)
// zu unterscheiden. Letzteres ist der forensisch wichtige Fall.
bool nvsLoadJson(const char* ns, const char* key, JsonDocument& doc,
                 bool* dataPresent = nullptr);

// Serialisiert 'doc' und schreibt als BLOB unter ns/key; Werte >= 512 B
// werden heatshrink-komprimiert (w=8/l=4, ~1,6 KB transienter Heap,
// Faktor ~2-3,6 auf Store-JSON), kleinere bleiben Klartext. Blob statt
// String, weil nvs_set_str hart bei 4000 B endet — das vernichtete ab
// ~5 Speakern jeden Save (Lab-Befund 2026-06-07).
bool nvsSaveJson(const char* ns, const char* key, JsonDocument& doc);

// Loescht einen Key.
bool nvsErase(const char* ns, const char* key);

// Eraset ALLE keys eines Namespaces.
bool nvsEraseAllInNamespace(const char* ns);

// Liefert NVS-Stats fuer die Default-Partition als JSON.
void nvsGetStatsJson(JsonDocument& out);

// Try-Save mit Auto-Cleanup-Fallback. Wenn der Blob-Write fehlschlaegt
// (NVS wirklich voll), werden Cache-Namespaces erased + retried. Pass 3
// sichert den alten Wert und stellt ihn bei erneutem Fehlschlag wieder
// her — vernichtet NIE den letzten guten Stand. Returns true bei Erfolg.
// Unchanged-Skip: ist der aktive A/B-Slot byte-identisch zum neuen Stand,
// wird gar nicht geschrieben (true) — per-Slice-Vollsweeps (PresetStore::
// saveToNVS, SpeakerInventory::saveToNVS) reichen auch ungeaenderte Slices
// durch, ohne Skip waere jeder Sweep N Flash-Writes statt der 1-2 echten.
bool nvsSaveJsonWithCleanup(const char* ns, const char* key, JsonDocument& doc);

// Enumeriert alle A/B-Basis-Keys eines Namespaces: "<id>", "<id>~",
// "<id>#0", "<id>#1" werden auf "<id>" reduziert und dedupliziert.
// excludeBaseIds (z.B. Legacy-Blob-Key, Order-Key) werden samt ihrer
// A/B-Geschwister uebersprungen. Fuer per-Slice-Stores, deren Key = eine
// Geraete-Id ist (PresetStore-Muster).
void nvsListAbBaseIds(const char* ns,
                      const char* const* excludeBaseIds, size_t excludeCount,
                      std::vector<String>& out);

// Loescht einen A/B-Basis-Key samt aller Geschwister ("<key>", "<key>~",
// "<key>#0", "<key>#1"). isKey-Guard je Einzel-Key — kein NOT_FOUND-E-Log
// fuer nie geschriebene Geschwister (die Zeilen haben in freds Serial-
// Mitschnitt msg1367457 fuer Verwirrung gesorgt). Reines Erase = die
// einzige Operation, die an der vollen Partition Platz SCHAFFT, ohne
// vorher schreiben zu muessen.
void nvsEraseAbKeys(const char* ns, const char* baseKey);

// Entry-Bedarf eines Blob-Writes von dlen Bytes — DIESELBE Formel, mit der
// das Write-Gate (writeBlobGated_) entscheidet. Fuer Vorab-Rechnungen wie
// den Inventory-Migrations-Guard: mit der Klartext-Laenge gefuettert ist
// das Ergebnis eine obere Schranke (Kompression macht dlen nur kleiner).
size_t nvsEntriesNeededForBlob(size_t dlen);

// Real fuer Daten nutzbare freie Entries: free_entries minus der permanent
// als GC-Spare reservierten Page (kNvsReservedPageEntries, s. nvs_helper.cpp).
// Liefert SIZE_MAX, wenn nvs_get_stats fehlschlaegt — Caller sollen dann
// nicht blocken (gleiches Verhalten wie das Write-Gate).
size_t nvsAvailableEntries();

// Entries, die ein A/B-Basis-Key aktuell belegt (beide Slots, sofern
// vorhanden, inkl. Blob-Index + Gen-Entries) — d.h. was ein
// nvsEraseAbKeys(ns, baseKey) wieder freigeben wuerde.
size_t nvsAbBlobEntries(const char* ns, const char* baseKey);

// Einmalige Daten-Migration BoseFix32 -> SixBack.
// Kopiert alle Keys (STR, U8, U16, U32, U64, I8, I16, I32, I64, BLOB) von
// `oldNs` nach `newNs`, falls `newNs` noch leer ist. Nach erfolgreicher
// Migration wird `oldNs` komplett geloescht. No-op wenn newNs schon Daten
// hat oder oldNs leer ist.
//
// MUSS in setup() VOR jedem loadFromNVS()-Aufruf passieren, sonst geht
// User-Konfig beim Rename-OTA-Update verloren (WiFi-Creds, Presets, Inv).
// Gibt true zurueck wenn migriert oder schon migriert; false bei Fehler.
bool migrateNvsNamespace(const char* oldNs, const char* newNs);

// Convenience: ruft migrateNvsNamespace fuer alle 7 BoseFix32-Namespaces
// und ihre SixBack-Pendants auf. Loggt Status pro Namespace.
void migrateAllBosefixNvs();

} // namespace sixback

#endif

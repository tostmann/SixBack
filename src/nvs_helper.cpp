// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nvs_helper.h"
#include <Preferences.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <memory>
#include <vector>

extern "C" {
#include "heatshrink/heatshrink_encoder.h"
#include "heatshrink/heatshrink_decoder.h"
}

namespace sixback {

namespace {

// ---------------------------------------------------------------------------
// Transparente Blob-Kompression (heatshrink, vendored src/heatshrink, ISC).
//
// Warum: die JSON-Stores (Presets, Inventory, Libraries) wachsen linear mit
// der Speaker-Zahl; unkomprimiert traegt die 24-KB-NVS-Partition ~7 voll
// belegte Speaker (Blob-Update haelt alt+neu gleichzeitig). On-device
// vermessen 2026-06-07: ROM-tdefl braucht 167.744 B Encoder-State — passt
// nicht in den Heap unter SixBack-Last auf C3/C6. heatshrink w=8/l=4:
// 1.596 B Encoder-State, 548 B Decoder-State, Faktor ~2-3,6 auf Store-JSON,
// 13 ms fuer 12 KB auf dem C6. Einheitlich auf allen 4 Chips.
//
// Blob-Format (Generationen-Kette, in-place-Migration beim ersten Save):
//   STRING-Typ            -> Legacy <= v0.8.14 (nvsLoadJson-Fallback)
//   Blob, beginnt mit '{' -> Klartext-JSON (v0.8.15/16 + kleine Werte)
//   Blob, beginnt mit "HS"-> [0]='H' [1]='S' [2]=(window<<4)|lookahead
//                            [3..6]=Klartextlaenge LE32, [7..]=heatshrink
// JSON beginnt immer mit '{' -> kollisionsfrei zum Magic.
// ---------------------------------------------------------------------------
constexpr uint8_t kHsWindow    = 8;   // 2^8-Fenster: 1,6 KB Encoder-State
constexpr uint8_t kHsLookahead = 4;
constexpr size_t  kHsHeader    = 7;
constexpr size_t  kCompressMin = 512;     // kleine Werte (TuneIn-Cache, Auth,
                                          // Diag) bleiben Klartext — Overhead
                                          // lohnt erst ab Store-Groesse
constexpr size_t  kPlainMax    = 65536;   // Sanity-Limit beim Laden (Partition
                                          // ist 24 KB; mehr = korrupt)

// Komprimiert 'in' in einen geframten Blob (out/outLen). false wenn nicht
// lohnend (Ergebnis nicht kleiner) oder OOM — Caller schreibt dann Klartext.
// Review-Härtung 2026-06-07: alle Buffer via new(std::nothrow) — das
// Framework baut mit -fexceptions, ein ungefangenes bad_alloc aus
// std::vector waere abort() statt des dokumentierten Klartext-Fallbacks.
// Beide Schleifen tragen einen Totalfortschritts-Guard (jede Iteration muss
// inPos ODER outPos bewegen), sonst sauberes false statt Haenger.
bool hsCompress_(const String& in, std::unique_ptr<uint8_t[]>& out, size_t& outLen) {
    const size_t inLen = in.length();
    if (inLen < kHsHeader + 1 || inLen > kPlainMax) return false;  // symmetrisch zum Load-Limit
    out.reset(new (std::nothrow) uint8_t[inLen]);
    if (!out) return false;
    heatshrink_encoder* hse = heatshrink_encoder_alloc(kHsWindow, kHsLookahead);
    if (!hse) { out.reset(); return false; }
    out[0] = 'H'; out[1] = 'S';
    out[2] = (uint8_t)((kHsWindow << 4) | kHsLookahead);
    out[3] = (uint8_t)(inLen & 0xFF);
    out[4] = (uint8_t)((inLen >> 8) & 0xFF);
    out[5] = (uint8_t)((inLen >> 16) & 0xFF);
    out[6] = (uint8_t)((inLen >> 24) & 0xFF);
    size_t inPos = 0, outPos = kHsHeader;
    bool ok = true;
    while (inPos < inLen && ok) {
        const size_t inPosBefore = inPos, outPosBefore = outPos;
        size_t n = 0;
        heatshrink_encoder_sink(hse, (uint8_t*)in.c_str() + inPos, inLen - inPos, &n);
        inPos += n;
        HSE_poll_res pr;
        do {
            if (outPos >= inLen) { ok = false; break; }   // wird nicht kleiner
            size_t o = 0;
            pr = heatshrink_encoder_poll(hse, out.get() + outPos, inLen - outPos, &o);
            outPos += o;
        } while (pr == HSER_POLL_MORE);
        if (ok && inPos == inPosBefore && outPos == outPosBefore) {
            ok = false;   // kein Fortschritt -> nie busy-spinnen
        }
    }
    while (ok) {
        HSE_finish_res fr = heatshrink_encoder_finish(hse);
        if (fr == HSER_FINISH_DONE) break;
        if (fr != HSER_FINISH_MORE) { ok = false; break; }
        if (outPos >= inLen)        { ok = false; break; }
        size_t o = 0;
        heatshrink_encoder_poll(hse, out.get() + outPos, inLen - outPos, &o);
        if (o == 0) { ok = false; break; }   // FINISH_MORE ohne Output = stuck
        outPos += o;
    }
    heatshrink_encoder_free(hse);
    if (!ok || outPos >= inLen) { out.reset(); return false; }
    outLen = outPos;
    return true;
}

// Entpackt einen "HS"-geframten Blob nach 'out' (NUL-terminiert, Laenge =
// plainLen aus dem Frame). Window/Lookahead kommen aus dem Frame —
// vorwaertskompatibel falls die Parameter je geaendert werden. false bei
// jedem Decode-/Konsistenz-/OOM-Fehler; haengt NIE (Totalfortschritts-Guard
// — Review-Befund 2026-06-07: ein inkonsistenter Frame mit zu kleinem
// plainLen liess die sink-Schleife sonst endlos spinnen -> WDT-Boot-Loop).
bool hsDecompress_(const uint8_t* in, size_t inLen, std::unique_ptr<char[]>& out) {
    if (inLen < kHsHeader || in[0] != 'H' || in[1] != 'S') return false;
    const uint8_t window    = in[2] >> 4;
    const uint8_t lookahead = in[2] & 0x0F;
    const size_t plainLen = (size_t)in[3] | ((size_t)in[4] << 8)
                          | ((size_t)in[5] << 16) | ((size_t)in[6] << 24);
    if (plainLen == 0 || plainLen > kPlainMax) {
        Serial.printf("[nvs-hs] reject: plainLen=%u\n", (unsigned)plainLen);
        return false;
    }
    if (window < 4 || window > 15 || lookahead < 2 || lookahead >= window) {
        Serial.printf("[nvs-hs] reject: params w=%u l=%u\n", window, lookahead);
        return false;
    }
    out.reset(new (std::nothrow) char[plainLen + 1]);
    if (!out) return false;
    heatshrink_decoder* hsd = heatshrink_decoder_alloc(256, window, lookahead);
    if (!hsd) { out.reset(); return false; }
    size_t cPos = kHsHeader, dPos = 0;
    bool ok = true;
    while (cPos < inLen && ok) {
        const size_t cPosBefore = cPos, dPosBefore = dPos;
        size_t n = 0;
        heatshrink_decoder_sink(hsd, (uint8_t*)in + cPos, inLen - cPos, &n);
        cPos += n;
        HSD_poll_res pr = HSDR_POLL_EMPTY;
        do {
            if (dPos >= plainLen) break;   // erwartete Laenge erreicht
            size_t o = 0;
            pr = heatshrink_decoder_poll(hsd, (uint8_t*)out.get() + dPos, plainLen - dPos, &o);
            dPos += o;
        } while (pr == HSDR_POLL_MORE);
        if (pr == HSDR_POLL_ERROR_NULL || pr == HSDR_POLL_ERROR_UNKNOWN) ok = false;
        if (ok && cPos == cPosBefore && dPos == dPosBefore) {
            // Kein Fortschritt: Input-Buffer voll (sink n=0) und Output am
            // (deklarierten) Ende, aber Komprimat uebrig -> Frame luegt ueber
            // plainLen. Sauber ablehnen statt WDT-Boot-Loop.
            Serial.println("[nvs-hs] reject: surplus input, frame inconsistent");
            ok = false;
        }
    }
    while (ok && dPos < plainLen) {
        HSD_finish_res fr = heatshrink_decoder_finish(hsd);
        if (fr != HSDR_FINISH_MORE) break;
        size_t o = 0;
        heatshrink_decoder_poll(hsd, (uint8_t*)out.get() + dPos, plainLen - dPos, &o);
        dPos += o;
        if (o == 0) break;
    }
    heatshrink_decoder_free(hsd);
    if (!ok || dPos != plainLen) {
        Serial.printf("[nvs-hs] decode FAIL: got %u, want %u\n",
                      (unsigned)dPos, (unsigned)plainLen);
        out.reset();
        return false;
    }
    out[plainLen] = '\0';
    return true;
}

} // namespace

// Definiert weiter unten beim A/B-Save; hier schon gebraucht.
static void slotKeys_(const char* key, String& k1, String& g0, String& g1);

// Laedt EINEN Slot (Blob, HS oder Klartext) — kein String-Fallback hier.
static bool loadSlotBlob_(Preferences& p, const char* ns, const char* key,
                          JsonDocument& doc) {
    size_t blen = p.getBytesLength(key);
    if (blen == 0) return false;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[blen + 1]);
    if (!buf) {
        Serial.printf("[nvs-load] FAIL alloc ns=%s key=%s blob=%u\n",
                      ns, key, (unsigned)blen);
        return false;
    }
    size_t got = p.getBytes(key, buf.get(), blen);
    if (got != blen) {
        Serial.printf("[nvs-load] FAIL getBytes ns=%s key=%s want=%u got=%u\n",
                      ns, key, (unsigned)blen, (unsigned)got);
        return false;
    }
    buf[blen] = '\0';
    // "HS"-Frame -> heatshrink-komprimiert; sonst Klartext-JSON-Blob.
    if (blen >= kHsHeader && buf[0] == 'H' && buf[1] == 'S') {
        std::unique_ptr<char[]> plain;
        if (!hsDecompress_(buf.get(), blen, plain)) {
            Serial.printf("[nvs-load] FAIL hs-decode ns=%s key=%s blob=%u\n",
                          ns, key, (unsigned)blen);
            return false;
        }
        // const char* erzwingt Copy-Mode in ArduinoJson.
        DeserializationError e = deserializeJson(doc, (const char*)plain.get());
        if (e != DeserializationError::Ok)
            Serial.printf("[nvs-load] FAIL json-parse ns=%s key=%s err=%s\n",
                          ns, key, e.c_str());
        return e == DeserializationError::Ok;
    }
    DeserializationError e = deserializeJson(doc, (const char*)buf.get());
    if (e != DeserializationError::Ok)
        Serial.printf("[nvs-load] FAIL json-parse ns=%s key=%s err=%s\n",
                      ns, key, e.c_str());
    return e == DeserializationError::Ok;
}

bool nvsLoadJson(const char* ns, const char* key, JsonDocument& doc,
                 bool* dataPresent) {
    if (dataPresent) *dataPresent = false;
    Preferences p;
    if (!p.begin(ns, true)) return false;
    // A/B-Slots (2026-07-17): kritische Stores liegen unter "<key>" ODER
    // "<key>~", die Generation ("<key>#0"/"#1") entscheidet welcher aktiv
    // ist. Einzel-Key-Stores (TuneIn-Cache etc.) haben nie Slot-1/Gen-Keys
    // -> identisches Verhalten wie vorher. Ist der hoeher-generierte Slot
    // unlesbar/unparsebar, faellt der Load auf den anderen zurueck
    // (Self-Heal auf den letzten guten Stand).
    String k1, g0k, g1k;
    slotKeys_(key, k1, g0k, g1k);
    uint32_t g0 = p.getUInt(g0k.c_str(), 0);
    uint32_t g1 = p.getUInt(g1k.c_str(), 0);
    const bool p0 = p.getBytesLength(key) > 0;
    const bool p1 = p.getBytesLength(k1.c_str()) > 0;
    if (p0 && g0 == 0) g0 = 1;   // Bestands-Blob ohne Gen-Entry (Legacy)
    if (p1 && g1 == 0) g1 = 1;
    if (dataPresent) *dataPresent = p0 || p1;

    const char* first  = (p1 && g1 > g0) ? k1.c_str() : key;
    const char* second = (first == key) ? (p1 ? k1.c_str() : nullptr)
                                        : (p0 ? key : nullptr);
    if (((first == key) ? p0 : p1) && loadSlotBlob_(p, ns, first, doc)) {
        p.end();
        return true;
    }
    if (second) {
        doc.clear();   // Parse-Reste des kaputten Slots nicht stehen lassen
        if (loadSlotBlob_(p, ns, second, doc)) {
            Serial.printf("[nvs-load] slot-fallback ns=%s key=%s -> %s (letzter guter Stand)\n",
                          ns, key, second);
            p.end();
            return true;
        }
    }
    if (p0 || p1) {   // Blob(s) vorhanden aber unlesbar -> harter Load-Fail
        p.end();
        return false;
    }
    // Bestands-Daten (<= v0.8.14) als STRING unter dem Basis-Key.
    String s = p.getString(key, "");
    p.end();
    if (s.length() == 0) return false;
    if (dataPresent) *dataPresent = true;
    DeserializationError e = deserializeJson(doc, s);
    if (e != DeserializationError::Ok)
        Serial.printf("[nvs-load] FAIL json-parse(str) ns=%s key=%s err=%s\n",
                      ns, key, e.c_str());
    return e == DeserializationError::Ok;
}

// Serialisiert + komprimiert das Dokument in einen schreibfertigen Blob.
// data/dlen zeigen entweder in 's' (Klartext) oder in 'packed' (HS-Frame).
static bool packPayload_(JsonDocument& doc, String& s,
                         std::unique_ptr<uint8_t[]>& packed,
                         const uint8_t*& data, size_t& dlen, bool& hs) {
    serializeJson(doc, s);
    if (s.length() == 0) return false;
    data = (const uint8_t*)s.c_str();
    dlen = s.length();
    hs   = false;
    size_t packedLen = 0;
    if (dlen >= kCompressMin && hsCompress_(s, packed, packedLen)) {
        data = packed.get();
        dlen = packedLen;
        hs   = true;
    }
    return true;
}

// Eine NVS-Page ist permanent als GC-Spare reserviert und nie fuer Daten
// nutzbar (IDF File-System-Considerations: "One page is always reserved
// and never used for data storage"). nvs_get_stats() zaehlt ihre Entries
// trotzdem als frei — free_entries ist also um genau eine Page zu
// optimistisch (4096 B / 32 B = 128, minus Page-Header + Entry-State-
// Bitmap = 126 Daten-Entries, NVS_CONST_ENTRY_COUNT im IDF). Das
// available_entries-Feld, das die Reserve rausrechnet, gibt es erst in
// neueren IDFs — der Arduino-Core hier hat es nicht, daher selbst abziehen.
static constexpr size_t kNvsReservedPageEntries = 126;

// Gate + putBytes + Logging. WRITE-GATE (Lab-Befund 2026-07-17): ein
// putBytes, das mangels Platz MITTEN im Multi-Chunk-Blob-Write abbricht,
// hinterlaesst einen inkonsistenten Blob — danach schlagen selbst
// Kleinst-Writes fehl (wrote=0 bei 792 B / 135 freien Entries beobachtet)
// und nvs_flash_init raeumt den Key beim NAECHSTEN BOOT komplett weg:
// Totalverlust mit Fresh-Install-Optik. Writes, die nicht sicher passen,
// werden gar nicht erst versucht. Der 07-17-Wedge ging durchs alte Gate,
// weil es gegen das ungefilterte free_entries prueft hat: 135 "frei" waren
// real 135-126=9 nutzbare Entries, der 792-B-Write brauchte ~25. Mit
// abgezogener Reserve haette das Gate ihn REFUSED. Rest-Unschaerfe
// (Chunk-Geometrie ueber Page-Grenzen) bleibt — deshalb ist die
// eigentliche Verlust-Absicherung weiterhin der A/B-Slot-Mechanismus
// (nvsSaveJsonAB_), das Gate reduziert die Wedge-Wahrscheinlichkeit.
static bool writeBlobGated_(Preferences& p, const char* ns, const char* key,
                            const uint8_t* data, size_t dlen,
                            size_t jsonLen, bool hs) {
    nvs_stats_t st{};
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
        const size_t needed = nvsEntriesNeededForBlob(dlen);
        const size_t avail  = st.free_entries > kNvsReservedPageEntries
                            ? st.free_entries - kNvsReservedPageEntries : 0;
        if (needed > avail) {
            Serial.printf("[nvs-save] REFUSED ns=%s key=%s blob=%u (brauche ~%u entries, "
                          "verfuegbar %u = frei %u - GC-Reserve %u) — Write nicht gestartet\n",
                          ns, key, (unsigned)dlen, (unsigned)needed,
                          (unsigned)avail, (unsigned)st.free_entries,
                          (unsigned)kNvsReservedPageEntries);
            return false;
        }
    }
    size_t n = p.putBytes(key, data, dlen);
    if (n != dlen) {
        Serial.printf("[nvs-save] FAIL putBytes ns=%s key=%s json_len=%u blob=%u wrote=%u%s\n",
                      ns, key, (unsigned)jsonLen, (unsigned)dlen, (unsigned)n,
                      hs ? " (hs)" : "");
        return false;
    }
    Serial.printf("[nvs-save] ok ns=%s key=%s json_len=%u blob=%u%s\n",
                  ns, key, (unsigned)jsonLen, (unsigned)dlen,
                  hs ? " (hs)" : " (plain)");
    return true;
}

bool nvsSaveJson(const char* ns, const char* key, JsonDocument& doc) {
    // BLOB statt String (Lab-Befund 2026-06-07): nvs_set_str kann max
    // 4000 B inkl. NUL — nvs_set_blob chunkt ueber Pages. Groessere Werte
    // heatshrink-komprimiert (Faktor ~2-3,6); Load erkennt beides am Frame.
    // Einzel-Key-Semantik: fuer kleine, regenerable Werte (TuneIn-Cache,
    // Diag). Kritische Stores gehen ueber nvsSaveJsonWithCleanup = A/B-Slots.
    String s;
    std::unique_ptr<uint8_t[]> packed;
    const uint8_t* data = nullptr;
    size_t dlen = 0;
    bool hs = false;
    if (!packPayload_(doc, s, packed, data, dlen, hs)) return false;
    Preferences p;
    if (!p.begin(ns, false)) {
        Serial.printf("[nvs-save] FAIL begin ns=%s key=%s\n", ns, key);
        return false;
    }
    bool ok = writeBlobGated_(p, ns, key, data, dlen, s.length(), hs);
    p.end();
    return ok;
}

// ---------------------------------------------------------------------------
// A/B-Slot-Save (Lab-Befund 2026-07-17, fred/144729 Totalverlust-Klasse):
// Der neue Stand wird IMMER in den inaktiven Slot geschrieben ("<key>" bzw.
// "<key>~"), nach Readback-Verify wird die Generation ("<key>#0"/"#1", u32 =
// atomarer Einzel-Entry) hochgezaehlt und erst dann der alte Slot geraeumt.
// Ein mittendrin scheiternder oder vom naechsten nvs_flash_init weggeraeumter
// Write kann so nur den SPARE-Slot treffen — der aktive Stand bleibt in jedem
// beobachteten Fehlermodus (wrote=0-Wedge, unlesbarer Blob, Init-Cleanup)
// konsistent und bootfest. Peak-Platzbedarf unveraendert (Update-in-place
// hielt alt+neu ebenfalls gleichzeitig).
// ---------------------------------------------------------------------------
static void slotKeys_(const char* key, String& k1, String& g0, String& g1) {
    k1 = String(key) + "~";
    g0 = String(key) + "#0";
    g1 = String(key) + "#1";
}

static bool nvsSaveJsonAB_(const char* ns, const char* key, JsonDocument& doc) {
    String s;
    std::unique_ptr<uint8_t[]> packed;
    const uint8_t* data = nullptr;
    size_t dlen = 0;
    bool hs = false;
    if (!packPayload_(doc, s, packed, data, dlen, hs)) return false;

    String k1, g0k, g1k;
    slotKeys_(key, k1, g0k, g1k);

    Preferences p;
    if (!p.begin(ns, false)) {
        Serial.printf("[nvs-save] FAIL begin ns=%s key=%s\n", ns, key);
        return false;
    }
    // Aktiven Slot bestimmen: hoehere Generation; Bestands-Blob ohne
    // Gen-Entry (Legacy/<= v0.8.33 oder NVS-Preseed) zaehlt als Gen 1.
    uint32_t g0 = p.getUInt(g0k.c_str(), 0);
    uint32_t g1 = p.getUInt(g1k.c_str(), 0);
    if (g0 == 0 && p.getBytesLength(key) > 0)          g0 = 1;
    if (g1 == 0 && p.getBytesLength(k1.c_str()) > 0)   g1 = 1;
    const bool  activeIs0 = g0 >= g1;
    const char* tgtKey    = activeIs0 ? k1.c_str() : key;
    const char* tgtGenKey = activeIs0 ? g1k.c_str() : g0k.c_str();
    const char* oldKey    = activeIs0 ? key : k1.c_str();
    const char* oldGenKey = activeIs0 ? g0k.c_str() : g1k.c_str();

    // Unchanged-Skip: ist der aktive Slot byte-identisch zum neuen Stand,
    // gibt es nichts zu schreiben — kein Flash-Wear, und an der vollen
    // Partition entfaellt der transiente alt+neu-Peak. Noetig, seit die
    // per-Slice-Vollsweeps (Inventory/PresetStore saveToNVS) auch
    // ungeaenderte Slices durchreichen. Kein Log — der Skip ist der
    // Normalfall im Sweep, das wuerde jede Serial-Session fluten.
    if ((g0 | g1) != 0) {
        size_t blen = p.getBytesLength(oldKey);
        if (blen == dlen && dlen > 0) {
            std::unique_ptr<uint8_t[]> cur(new (std::nothrow) uint8_t[dlen]);
            if (cur && p.getBytes(oldKey, cur.get(), dlen) == dlen &&
                memcmp(cur.get(), data, dlen) == 0) {
                p.end();
                return true;
            }
        }
    }

    // Ziel-Slot raeumen (Stale-Reste von vorvorigem Save) — inhaerent safe,
    // der aktive Slot wird hier NIE angefasst.
    p.remove(tgtKey);
    if (!writeBlobGated_(p, ns, tgtKey, data, dlen, s.length(), hs)) {
        p.end();
        return false;
    }
    // Readback-Verify: nur ein nachweislich vollstaendig lesbarer Spare darf
    // den aktiven Stand abloesen (Lab: putBytes meldete Erfolg, getBytes
    // scheiterte anschliessend).
    bool verified = false;
    {
        size_t blen = p.getBytesLength(tgtKey);
        if (blen == dlen) {
            std::unique_ptr<uint8_t[]> rb(new (std::nothrow) uint8_t[dlen]);
            if (rb && p.getBytes(tgtKey, rb.get(), dlen) == dlen &&
                memcmp(rb.get(), data, dlen) == 0) {
                verified = true;
            }
        }
    }
    if (!verified) {
        Serial.printf("[nvs-save] FAIL readback ns=%s key=%s blob=%u — Spare verworfen, "
                      "aktiver Stand unangetastet\n", ns, tgtKey, (unsigned)dlen);
        p.remove(tgtKey);
        p.end();
        return false;
    }
    uint32_t newGen = (g0 > g1 ? g0 : g1) + 1;
    if (p.putUInt(tgtGenKey, newGen) == 0) {
        // Gen-Commit fehlgeschlagen -> Load wuerde weiter den alten Slot
        // bevorzugen. Spare aufraeumen, Save als gescheitert melden.
        Serial.printf("[nvs-save] FAIL gen-commit ns=%s key=%s\n", ns, tgtGenKey);
        p.remove(tgtKey);
        p.end();
        return false;
    }
    // Alten Slot freigeben (best effort — scheitert das, verliert er beim
    // Load ohnehin gegen die hoehere Generation).
    p.remove(oldKey);
    p.remove(oldGenKey);
    p.end();
    return true;
}

bool nvsErase(const char* ns, const char* key) {
    Preferences p;
    if (!p.begin(ns, false)) return false;
    bool ok = p.remove(key);
    p.end();
    return ok;
}

// "<id>", "<id>~", "<id>#0", "<id>#1" -> "<id>" (die A/B-Suffixe aus
// slotKeys_). Alles andere unveraendert.
static String abBaseIdFromKey_(const char* k) {
    String s(k);
    if (s.endsWith("~")) return s.substring(0, s.length() - 1);
    if (s.length() >= 2 && s.charAt(s.length() - 2) == '#') {
        const char c = s.charAt(s.length() - 1);
        if (c == '0' || c == '1') return s.substring(0, s.length() - 2);
    }
    return s;
}

void nvsListAbBaseIds(const char* ns,
                      const char* const* excludeBaseIds, size_t excludeCount,
                      std::vector<String>& out) {
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        String base = abBaseIdFromKey_(info.key);
        bool skip = base.length() == 0;
        for (size_t i = 0; !skip && i < excludeCount; ++i) {
            if (base == excludeBaseIds[i]) skip = true;
        }
        // Dedupe linear — die Stores halten <= ~20 Basis-Ids.
        if (!skip) {
            for (auto& b : out) { if (b == base) { skip = true; break; } }
        }
        if (!skip) out.push_back(base);
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
}

void nvsEraseAbKeys(const char* ns, const char* baseKey) {
    Preferences p;
    if (!p.begin(ns, false)) return;
    const String k1 = String(baseKey) + "~";
    const String g0 = String(baseKey) + "#0";
    const String g1 = String(baseKey) + "#1";
    if (p.isKey(baseKey))    p.remove(baseKey);
    if (p.isKey(k1.c_str())) p.remove(k1.c_str());
    if (p.isKey(g0.c_str())) p.remove(g0.c_str());
    if (p.isKey(g1.c_str())) p.remove(g1.c_str());
    p.end();
}

size_t nvsEntriesNeededForBlob(size_t dlen) {
    return dlen / 32 + (dlen / 4000) * 2 + 10;
}

size_t nvsAvailableEntries() {
    nvs_stats_t st{};
    if (nvs_get_stats(NULL, &st) != ESP_OK) return SIZE_MAX;
    return st.free_entries > kNvsReservedPageEntries
         ? st.free_entries - kNvsReservedPageEntries : 0;
}

size_t nvsAbBlobEntries(const char* ns, const char* baseKey) {
    Preferences p;
    if (!p.begin(ns, true)) return 0;
    const String k1 = String(baseKey) + "~";
    const String g0 = String(baseKey) + "#0";
    const String g1 = String(baseKey) + "#1";
    size_t entries = 0;
    size_t l0 = p.isKey(baseKey)    ? p.getBytesLength(baseKey)    : 0;
    size_t l1 = p.isKey(k1.c_str()) ? p.getBytesLength(k1.c_str()) : 0;
    if (l0) entries += (l0 + 31) / 32 + 1;   // Daten-Chunks + BLOB_IDX
    if (l1) entries += (l1 + 31) / 32 + 1;
    if (p.isKey(g0.c_str())) entries += 1;   // Gen-Entries (u32 = 1 Entry)
    if (p.isKey(g1.c_str())) entries += 1;
    p.end();
    return entries;
}

bool nvsEraseAllInNamespace(const char* ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_erase_all(h);
    esp_err_t e2 = nvs_commit(h);
    nvs_close(h);
    return e1 == ESP_OK && e2 == ESP_OK;
}

void nvsGetStatsJson(JsonDocument& out) {
    nvs_stats_t st{};
    esp_err_t e = nvs_get_stats(NULL, &st);
    if (e != ESP_OK) {
        out["error"] = "nvs_get_stats failed";
        return;
    }
    out["used_entries"]      = (uint32_t)st.used_entries;
    out["free_entries"]      = (uint32_t)st.free_entries;
    out["total_entries"]     = (uint32_t)st.total_entries;
    out["namespace_count"]   = (uint32_t)st.namespace_count;
    out["percent_used"]      = (st.total_entries > 0)
                                ? (100.0f * st.used_entries / st.total_entries)
                                : 0.0f;
}

bool nvsSaveJsonWithCleanup(const char* ns, const char* key, JsonDocument& doc) {
    // Kritische Stores: IMMER ueber den A/B-Slot-Save (siehe nvsSaveJsonAB_)
    // — ein fehlschlagender Write kann damit konstruktionsbedingt nur den
    // Spare-Slot treffen, nie den aktiven Stand. Der fruehere Pass 3
    // (Ziel-Key erasen + Backup/Restore) ist damit obsolet: er hat im Lab
    // (2026-07-17, 42-Delete-Storm ueber der 9-Speaker-Kante) den letzten
    // guten Stand trotz Restore-Logik zweimal komplett verloren (unlesbarer
    // Blob nach putBytes-Abbruch -> nvs_flash_init raeumt den Key beim
    // naechsten Boot weg, Fresh-Install-Optik).
    if (nvsSaveJsonAB_(ns, key, doc)) return true;
    Serial.printf("[nvs-cleanup] %s/%s save fail — pass1 purging caches + retry\n", ns, key);
    // Pass 1: Cache-Namespaces wegpurgen (regenerable, kein User-Verlust).
    nvsEraseAllInNamespace("sixback-tune");      // TuneIn-Resolver-Cache
    nvsEraseAllInNamespace("sixback-sys");       // Health/Counters
    if (nvsSaveJsonAB_(ns, key, doc)) {
        Serial.printf("[nvs-cleanup] pass1 retry %s/%s -> OK\n", ns, key);
        return true;
    }
    // Pass 2: aggressivere Liste — Snapshot-Persistenz, Keepalive-State.
    // Beides regenerable beim naechsten Pre-Migrate / Boot. Loescht nicht
    // sixback-{pre,inv,spot,wifi,auto,net} — die haben User-Daten.
    Serial.printf("[nvs-cleanup] %s/%s pass1-fail — pass2 wider purge\n", ns, key);
    nvsEraseAllInNamespace("sixback-snapshot");  // Diag-Snapshots im Flash
    nvsEraseAllInNamespace("sixback-keepalive"); // Marge-Keepalive-State
    nvsEraseAllInNamespace("sixback-c");         // Captive-Stub-State
    nvsEraseAllInNamespace("sixback-s");         // Speaker-Discovery-Cache
    nvsEraseAllInNamespace("sixback-esp");       // ESP-Web-Tools-Cache (falls vorhanden)
    if (nvsSaveJsonAB_(ns, key, doc)) {
        Serial.printf("[nvs-cleanup] pass2 retry %s/%s -> OK (aggressive purge)\n", ns, key);
        return true;
    }
    Serial.printf("[nvs-cleanup] %s/%s -> STILL-FAIL — NVS partition genuinely full "
                  "(aktiver Stand bleibt konsistent)\n", ns, key);
    return false;
}

namespace {

// Liefert true wenn der Namespace mindestens einen Eintrag hat.
bool nvsNamespaceHasEntries(const char* ns) {
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
    if (err != ESP_OK || it == nullptr) {
        if (it) nvs_release_iterator(it);
        return false;
    }
    nvs_release_iterator(it);
    return true;
}

// Kopiert einen einzelnen Eintrag basierend auf seinem Typ. Gibt true bei
// Erfolg. Auf Fehler wird der Eintrag uebersprungen und false zurueckgegeben.
bool copyEntry(nvs_handle_t src, nvs_handle_t dst, const nvs_entry_info_t& info) {
    switch (info.type) {
        case NVS_TYPE_U8:  { uint8_t  v; if (nvs_get_u8 (src, info.key, &v) == ESP_OK) return nvs_set_u8 (dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I8:  { int8_t   v; if (nvs_get_i8 (src, info.key, &v) == ESP_OK) return nvs_set_i8 (dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U16: { uint16_t v; if (nvs_get_u16(src, info.key, &v) == ESP_OK) return nvs_set_u16(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I16: { int16_t  v; if (nvs_get_i16(src, info.key, &v) == ESP_OK) return nvs_set_i16(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U32: { uint32_t v; if (nvs_get_u32(src, info.key, &v) == ESP_OK) return nvs_set_u32(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I32: { int32_t  v; if (nvs_get_i32(src, info.key, &v) == ESP_OK) return nvs_set_i32(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_U64: { uint64_t v; if (nvs_get_u64(src, info.key, &v) == ESP_OK) return nvs_set_u64(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_I64: { int64_t  v; if (nvs_get_i64(src, info.key, &v) == ESP_OK) return nvs_set_i64(dst, info.key, v) == ESP_OK; break; }
        case NVS_TYPE_STR: {
            size_t len = 0;
            if (nvs_get_str(src, info.key, nullptr, &len) != ESP_OK || len == 0) return false;
            char* tmp = (char*)malloc(len);
            if (!tmp) return false;
            bool ok = nvs_get_str(src, info.key, tmp, &len) == ESP_OK
                   && nvs_set_str(dst, info.key, tmp) == ESP_OK;
            free(tmp);
            return ok;
        }
        case NVS_TYPE_BLOB: {
            size_t len = 0;
            if (nvs_get_blob(src, info.key, nullptr, &len) != ESP_OK || len == 0) return false;
            uint8_t* tmp = (uint8_t*)malloc(len);
            if (!tmp) return false;
            bool ok = nvs_get_blob(src, info.key, tmp, &len) == ESP_OK
                   && nvs_set_blob(dst, info.key, tmp, len) == ESP_OK;
            free(tmp);
            return ok;
        }
        default:
            return false;
    }
    return false;
}

} // namespace

bool migrateNvsNamespace(const char* oldNs, const char* newNs) {
    // No-op wenn alte Daten gar nicht da sind.
    if (!nvsNamespaceHasEntries(oldNs)) return true;

    // Idempotent: wenn neue Namespace schon Daten hat, gilt Migration als
    // abgeschlossen. Wir loeschen die alte aber NICHT — Sicherheits-Backup
    // bleibt einen Boot lang erhalten falls beim Schreiben in neue NS was
    // schiefging und der naechste Boot retry will. Erst beim 2. Boot mit
    // sauberer neuer NS wird die alte tot.
    if (nvsNamespaceHasEntries(newNs)) {
        // Beide vorhanden: alte ist Cruft, wegputzen.
        nvs_handle_t h;
        if (nvs_open(oldNs, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
            Serial.printf("[nvs-migrate] %s already migrated, erased leftover\n", oldNs);
        }
        return true;
    }

    nvs_handle_t src = 0, dst = 0;
    if (nvs_open(oldNs, NVS_READWRITE, &src) != ESP_OK) {
        Serial.printf("[nvs-migrate] open %s failed\n", oldNs);
        return false;
    }
    if (nvs_open(newNs, NVS_READWRITE, &dst) != ESP_OK) {
        Serial.printf("[nvs-migrate] open %s failed\n", newNs);
        nvs_close(src);
        return false;
    }

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find("nvs", oldNs, NVS_TYPE_ANY, &it);
    int copied = 0, failed = 0;
    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        if (copyEntry(src, dst, info)) {
            copied++;
        } else {
            failed++;
            Serial.printf("[nvs-migrate] copy fail %s/%s (type=%d)\n",
                          oldNs, info.key, (int)info.type);
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);

    if (failed == 0 && copied > 0) {
        nvs_commit(dst);
        nvs_erase_all(src);
        nvs_commit(src);
        Serial.printf("[nvs-migrate] %s -> %s (%d keys)\n", oldNs, newNs, copied);
    } else if (copied > 0) {
        nvs_commit(dst);
        Serial.printf("[nvs-migrate] %s -> %s partial (%d ok, %d fail, OLD KEPT)\n",
                      oldNs, newNs, copied, failed);
    }
    nvs_close(src);
    nvs_close(dst);
    return failed == 0;
}

void migrateAllBosefixNvs() {
    static const struct { const char* oldNs; const char* newNs; } map[] = {
        { "bosefix-wifi", "sixback-wifi" },  // ZUERST: WiFi-Creds entscheiden Live-Status
        { "bosefix-pre",  "sixback-pre"  },  // User-Presets
        { "bosefix-inv",  "sixback-inv"  },  // Speaker-Inventory
        { "bosefix-auto", "sixback-auto" },  // Auto-Mode-Settings
        { "bosefix-net",  "sixback-net"  },  // IP-Failsafe
        { "bosefix-sys",  "sixback-sys"  },  // Health-Counters
        { "bosefix-tune", "sixback-tune" },  // TuneIn-Cache (unkritisch)
    };
    for (auto& m : map) {
        migrateNvsNamespace(m.oldNs, m.newNs);
    }
}

} // namespace sixback

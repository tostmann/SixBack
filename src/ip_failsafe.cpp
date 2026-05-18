// SPDX-License-Identifier: GPL-3.0-or-later
#include "ip_failsafe.h"
#include "config.h"
#include "speaker_inventory.h"
#include "speaker_telnet.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>

namespace bosefix {

namespace {
constexpr const char* NVS_NS  = "bosefix-net";
constexpr const char* KEY_IP  = "last_ip";

// GET http://<ip>:8090/info → margeURL extrahieren.
// Leerstring bei Timeout / HTTP-Fehler / kein Tag — Caller behandelt das
// wie "unknown" und re-migriert vorsichtshalber.
String fetchMargeUrl_(const String& ip) {
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(1500); http.setTimeout(2500);
    String url = "http://" + ip + ":" + String(BOSE_BMX_PORT) + "/info";
    if (!http.begin(url)) return "";
    int code = http.GET();
    if (code != 200) { http.end(); return ""; }
    String xml = http.getString();
    http.end();
    int b = xml.indexOf("<margeURL>");
    if (b < 0) return "";
    b += 10;  // strlen("<margeURL>")
    int e = xml.indexOf("</margeURL>", b);
    if (e < 0) return "";
    return xml.substring(b, e);
}
} // anon

String getLastKnownIp() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return "";
    String s = p.getString(KEY_IP, "");
    p.end();
    return s;
}

static void rememberIp(const String& ip) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putString(KEY_IP, ip);
    p.end();
}

void ipFailsafeCheck() {
    String now = WiFi.localIP().toString();
    String prev = getLastKnownIp();
    String newBase = "http://" + now + ":" + String(BOSE_HTTP_PORT);

    if (prev.length() == 0) {
        Serial.printf("[failsafe] first boot, IP=%s persisted\n", now.c_str());
        rememberIp(now);
        return;
    }
    if (prev == now) {
        Serial.printf("[failsafe] IP unchanged (%s) — nothing to do\n", now.c_str());
        return;
    }
    Serial.printf("[failsafe] IP changed: %s -> %s — checking owned speakers\n",
                  prev.c_str(), now.c_str());
    auto& inv = SpeakerInventory::instance();
    int touched = 0, failed = 0, skipped = 0;
    // Iteriere ueber OwnedByUs (NICHT status), weil Status nach Reboot evtl.
    // noch nicht refreshed ist und ein migrated-Speaker mit veralteter IP
    // bereits 'unknown' aussieht.
    auto speakers = inv.list();
    for (auto& s : speakers) {
        if (!s.ownedByUs) continue;
        // Pre-Probe: zeigt der Speaker LIVE schon auf unsere neue Base?
        // Erspart einen Reboot, der nichts aendert.
        String live = fetchMargeUrl_(s.ip);
        if (live == newBase) {
            Serial.printf("[failsafe]   %s (%s) already on %s — skip\n",
                          s.name.c_str(), s.ip.c_str(), newBase.c_str());
            {
                SpeakerInventory::LockGuard g(inv);
                if (auto* p = inv.findById(s.deviceId)) {
                    p->status   = MigrationStatus::MIGRATED;
                    p->cloudUrl = newBase;
                }
            }
            ++skipped;
            continue;
        }
        Serial.printf("[failsafe]   %s (%s) live=%s — re-migrate to %s ...\n",
                      s.name.c_str(), s.ip.c_str(),
                      live.length() ? live.c_str() : "?(unreachable)",
                      newBase.c_str());
        auto r = migrateSpeaker(s.ip, newBase);
        if (r.ok) {
            ++touched;
            SpeakerInventory::LockGuard g(inv);
            if (auto* p = inv.findById(s.deviceId)) {
                p->status   = MigrationStatus::MIGRATED;
                p->cloudUrl = newBase;
            }
        } else {
            ++failed;
            Serial.printf("[failsafe]     FAILED: %s\n", r.message.c_str());
        }
    }
    inv.saveToNVS();
    Serial.printf("[failsafe] done: %d re-migrated, %d skipped (already-up-to-date), %d failed\n",
                  touched, skipped, failed);
    rememberIp(now);
}

} // namespace bosefix

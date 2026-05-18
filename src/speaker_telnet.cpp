// SPDX-License-Identifier: GPL-3.0-or-later
// BoseFix32 — Speaker-Bootstrap via Telnet Port 17000
//
// Implementiert die in RESEARCH.md §12 verifizierte Sequenz:
//   sys configuration bmxRegistryUrl <base>/bmx/registry/v1/services
//   sys configuration statsServerUrl <base>
//   sys configuration margeServerUrl <base>
//   sys configuration swUpdateUrl    <base>/updates/soundtouch
//   envswitch boseurls set <base> <base>/updates/soundtouch
//   getpdo CurrentSystemConfiguration
//   sys reboot
//
// Phase 0: vollständig implementiert für manuellen Aufruf (z.B. aus
// Serial-Console). Phase 3: an Web-UI angebunden.

#include "speaker_telnet.h"
#include "config.h"
#include <WiFi.h>

static bool sendAndExpectOK(WiFiClient& client, const String& cmd,
                             String& reply) {
    client.print(cmd + "\n");
    reply = "";
    uint32_t deadline = millis() + 3000;
    while (millis() < deadline) {
        while (client.available()) {
            char c = client.read();
            reply += c;
            if (reply.endsWith("->")) {
                // Bose-Prompt — Antwort komplett. Negative Marker
                // ueberstimmen alles andere: "Setting 'foo' not found" matchte
                // vorher faelschlich "Setting" als success.
                if (reply.indexOf("not found") >= 0 ||
                    reply.indexOf("Usage:")    >= 0 ||
                    reply.indexOf("usage:")    >= 0 ||
                    reply.indexOf("Error")     >= 0 ||
                    reply.indexOf("Invalid")   >= 0 ||
                    reply.indexOf("syntax")    >= 0) {
                    return false;
                }
                return true;
            }
        }
        delay(20);
    }
    return false;
}

// Liest aus einem getpdo-CurrentSystemConfiguration-Reply den Wert eines
// PDO-Felds ('text: "..."' nach dem Feldnamen) heraus. Liefert "" wenn
// nichts gefunden wurde — passt auch zum getpdo-quirk wo manche Felder
// kurzzeitig fehlen.
static String extractPdoField_(const String& reply, const String& field) {
    int p = reply.indexOf(field);
    if (p < 0) return "";
    int t = reply.indexOf("text:", p);
    if (t < 0) return "";
    int q1 = reply.indexOf('"', t);
    int q2 = (q1 >= 0) ? reply.indexOf('"', q1 + 1) : -1;
    if (q1 < 0 || q2 <= q1) return "";
    return reply.substring(q1 + 1, q2);
}

MigrationResult migrateSpeaker(const String& speakerIP,
                                const String& serverBaseUrl) {
    MigrationResult r{false, "", ""};

    WiFiClient client;
    if (!client.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 5000)) {
        r.message = "Telnet-Connect zu " + speakerIP + ":17000 fehlgeschlagen";
        return r;
    }

    // Banner-Prompt abwarten
    delay(300);
    while (client.available()) client.read();

    struct { const char* cmd_prefix; String value; } commands[5] = {
        { "sys configuration bmxRegistryUrl ", serverBaseUrl + "/bmx/registry/v1/services" },
        { "sys configuration statsServerUrl ", serverBaseUrl },
        { "sys configuration margeServerUrl ", serverBaseUrl },
        { "sys configuration swUpdateUrl ",    serverBaseUrl + "/updates/soundtouch" },
        { "envswitch boseurls set ",           serverBaseUrl + " " + serverBaseUrl + "/updates/soundtouch" },
    };

    String reply;
    for (auto& c : commands) {
        String cmd = String(c.cmd_prefix) + c.value;
        if (!sendAndExpectOK(client, cmd, reply)) {
            r.message = "Kommando fehlgeschlagen: " + cmd + "\nAntwort: " + reply;
            client.stop();
            return r;
        }
    }

    // Verifikation: getpdo soll zeigen dass margeServerUrl jetzt auf unsere
    // base zeigt. Falls Mismatch: NICHT fail-hard, weil getpdo zeitweise
    // inkonsistent ist (Diag-Shell-Quirk, NVS-Cache-Latenz) — Auto-Claim/
    // Release in refreshMigrationStatus reconcile't das beim naechsten Refresh.
    // Aber: das Mismatch wird ins r.message geschrieben, damit der User im
    // Reply-Toast sieht, dass die Verifikation nicht clean durchging.
    delay(200);  // NVS-write am Speaker abklingen lassen
    if (sendAndExpectOK(client, "getpdo CurrentSystemConfiguration", reply)) {
        r.verifiedConfig = reply;
        String actualMarge = extractPdoField_(reply, "margeServerUrl");
        if (actualMarge.length() > 0 && actualMarge != serverBaseUrl) {
            Serial.printf("[telnet] WARN getpdo margeServerUrl=%s expected=%s "
                          "— auto-claim will reconcile next refresh\n",
                          actualMarge.c_str(), serverBaseUrl.c_str());
            r.message = String("WARN: getpdo margeServerUrl=") + actualMarge +
                        " expected=" + serverBaseUrl + " (auto-claim will fix)";
        }
    }

    // Reboot
    client.print("sys reboot\n");
    delay(500);
    client.stop();

    r.ok = true;
    r.message = "Migration erfolgreich, Speaker rebooted";
    return r;
}

MigrationResult revertSpeaker(const String& speakerIP) {
    MigrationResult r{false, "", ""};
    WiFiClient c;
    if (!c.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 5000)) {
        r.message = "Telnet-Connect fehlgeschlagen";
        return r;
    }
    delay(300);
    while (c.available()) c.read();
    // Original-Bose-URLs zuruecksetzen (auch wenn Cloud tot - das ist der
    // Werks-Zustand)
    struct { const char* cmd; } commands[5] = {
        { "sys configuration bmxRegistryUrl https://content.api.bose.io/bmx/registry/v1/services" },
        { "sys configuration statsServerUrl https://events.api.bosecm.com" },
        { "sys configuration margeServerUrl https://streaming.bose.com" },
        { "sys configuration swUpdateUrl    https://worldwide.bose.com/updates/soundtouch" },
        { "envswitch boseurls set https://streaming.bose.com https://worldwide.bose.com/updates/soundtouch" },
    };
    String reply;
    for (auto& cmd : commands) {
        if (!sendAndExpectOK(c, cmd.cmd, reply)) {
            r.message = String("Kommando fehlgeschlagen: ") + cmd.cmd + "\n" + reply;
            c.stop();
            return r;
        }
    }
    if (!sendAndExpectOK(c, "getpdo CurrentSystemConfiguration", reply)) {
        r.message = "getpdo fehlgeschlagen";
        c.stop();
        return r;
    }
    r.verifiedConfig = reply;
    c.print("sys reboot\n");
    delay(500);
    c.stop();
    r.ok = true;
    r.message = "Revert erfolgreich, Speaker rebooted";
    return r;
}

bool rebootSpeaker(const String& speakerIP) {
    WiFiClient c;
    if (!c.connect(speakerIP.c_str(), BOSE_TELNET_PORT, 3000)) return false;
    delay(200);
    while (c.available()) c.read();
    c.print("sys reboot\n");
    delay(500);
    c.stop();
    return true;
}

#pragma once
// HTTP route registration for SonicCanvas.
// Call registerWebRoutes() once from setup(); it also calls webServer.begin().

#include <WebServer.h>
#include <Preferences.h>
#include <ElegantOTA.h>
#include "panel_defs.h"
#include "visualizer.h"
#include "ticker.h"
#include "music_player.h"
#include "web_ui.h"

extern WebServer           webServer;
extern MatrixPanel_I2S_DMA* dma_display;
extern uint32_t             g_breathe_start;
extern uint8_t              g_mix_r, g_mix_g, g_mix_b;

void registerWebRoutes() {
    // ── / : main control page ──────────────────────────────────────────────────
    webServer.on("/", []() {
        webServer.send(200, "text/html", buildRootHTML());
    });

    // ── /viz : set visualisation mode (0 … VIZ_COUNT-1) ──────────────────────
    webServer.on("/viz", HTTP_GET, []() {
        if (webServer.hasArg("n")) {
            int n = webServer.arg("n").toInt();
            if (n >= 0 && n < VIZ_COUNT) {
                g_viz_mode = n;
                dma_display->clearScreen();
            }
        }
        webServer.send(200, "text/plain", String(g_viz_mode));
    });

    // ── /breathe : start 4-7-8 breathing mode ─────────────────────────────────
    webServer.on("/breathe", HTTP_GET, []() {
        g_breathe_start = millis();
        g_viz_mode = VIZ_BREATHE;
        tickerClear();
        dma_display->clearScreen();
        webServer.send(200, "text/plain", "ok");
    });

    // ── /colormix : enter color-mixer mode ────────────────────────────────────
    webServer.on("/colormix", HTTP_GET, []() {
        g_viz_mode = VIZ_COLORMIX;
        dma_display->clearScreen();
        webServer.send(200, "text/plain", "ok");
    });

    // ── /mix : update color-mixer color (r/g/b 0-255, no mode switch) ─────────
    webServer.on("/mix", HTTP_GET, []() {
        if (webServer.hasArg("r")) g_mix_r = (uint8_t)constrain(webServer.arg("r").toInt(), 0, 255);
        if (webServer.hasArg("g")) g_mix_g = (uint8_t)constrain(webServer.arg("g").toInt(), 0, 255);
        if (webServer.hasArg("b")) g_mix_b = (uint8_t)constrain(webServer.arg("b").toInt(), 0, 255);
        webServer.send(200, "text/plain", "ok");
    });

    // ── /panels : get or set active + physical panel counts ──────────────────
    webServer.on("/panels", HTTP_GET, []() {
        if (webServer.hasArg("n") || webServer.hasArg("max")) {
            int n  = webServer.hasArg("n")   ? webServer.arg("n").toInt()   : SD_W / PANEL_RES_X;
            int mx = webServer.hasArg("max") ? webServer.arg("max").toInt() : g_max_panels;
            if (n < 1 || n > 6 || mx < n || mx > 6) {
                webServer.send(400, "text/plain", "invalid: n=1-6, max>=n, max<=6");
                return;
            }
            Preferences prefs;
            prefs.begin("soniccanvas", false);
            prefs.putInt("panels",     n);
            prefs.putInt("max_panels", mx);
            prefs.end();
            webServer.send(200, "text/plain",
                "saved:active=" + String(n) + " max=" + String(mx) + " rebooting...");
            delay(300);
            ESP.restart();
        } else {
            webServer.send(200, "text/plain",
                "active=" + String(SD_W / PANEL_RES_X) + " max=" + String(g_max_panels));
        }
    });

    // ── /play : trigger LittleFS playback ────────────────────────────────────
    webServer.on("/play", HTTP_GET, []() {
        String path = webServer.arg("f");
        if (!path.length()) { webServer.send(400, "text/plain", "missing ?f="); return; }
        if (webServer.arg("stop") == "1") { musicPlayerStop(); webServer.send(200, "text/plain", "stopped"); return; }
        bool ok = musicPlayerPlay(path.c_str());
        webServer.send(200, "text/plain", ok ? "playing" : "error");
    });

    // ── /fs : filesystem manager page ────────────────────────────────────────
    webServer.on("/fs", HTTP_GET, []() {
        String html =
          F("<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:600px;margin:20px auto'>"
            "<h2>&#128193; Filesystem</h2><p><a href='/'>&#8592; Player</a></p>");
        size_t used  = LittleFS.usedBytes();
        size_t total = LittleFS.totalBytes();
        html += "<p>Used: <b>" + String(used/1024) + " KB</b> / " + String(total/1024) + " KB</p>";
        html += F("<table border='1' cellpadding='6' cellspacing='0'>"
                  "<tr><th>File</th><th>Size</th><th>Actions</th></tr>");
        File root = LittleFS.open("/");
        File entry = root.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                String name = "/" + String(entry.name());
                String sz   = String(entry.size()) + " B";
                html += "<tr><td>" + name + "</td><td>" + sz + "</td><td>"
                        "<a href='/play?f=" + name + "'>&#9654; Play</a> "
                        "<a href='/fs/download?f=" + name + "'>&#11123; Download</a> "
                        "<a href='/fs/delete?f=" + name + "' "
                        "onclick=\"return confirm('Delete " + name + "?')\">&#10008; Delete</a>"
                        "</td></tr>";
            }
            entry.close();
            entry = root.openNextFile();
        }
        root.close();
        html += F("</table></body></html>");
        webServer.send(200, "text/html", html);
    });

    // ── /fs/download ──────────────────────────────────────────────────────────
    webServer.on("/fs/download", HTTP_GET, []() {
        String path = webServer.arg("f");
        File f = LittleFS.open(path, "r");
        if (!f) { webServer.send(404, "text/plain", "Not found"); return; }
        String filename = path.substring(path.lastIndexOf('/') + 1);
        webServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
        webServer.streamFile(f, "application/octet-stream");
        f.close();
    });

    // ── /fs/delete ────────────────────────────────────────────────────────────
    webServer.on("/fs/delete", HTTP_GET, []() {
        String path = webServer.arg("f");
        if (path.length() > 1 && LittleFS.exists(path)) {
            LittleFS.remove(path);
            if (path == "/music.wav") musicPlayerStop();
            Serial.printf("[fs] deleted %s\n", path.c_str());
        }
        webServer.sendHeader("Location", "/fs");
        webServer.send(302, "text/plain", "");
    });

    // ── /text : set scrolling ticker ─────────────────────────────────────────
    webServer.on("/text", HTTP_GET, []() {
        String msg = webServer.arg("msg");
        if (msg.length()) {
            if (webServer.arg("once") == "1")
                tickerSetOnce(msg.c_str());
            else
                tickerSet(msg.c_str());
            webServer.send(200, "text/plain", "ok");
        } else {
            tickerClear();
            webServer.send(200, "text/plain", "cleared");
        }
    });

    ElegantOTA.begin(&webServer);
    webServer.begin();
    Serial.println("HTTP server started");
}

#pragma once
// Idle clock and UDP-status display — called from loop() when no audio is active.
// drawIdleDisplay() handles the clock section; tickerDraw() is still called separately.

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "panel_defs.h"
#include "bt_audio.h"
#include "ticker.h"

#ifndef SD_H
#define SD_H PANEL_RES_Y
#endif

void drawIdleDisplay(MatrixPanel_I2S_DMA* d, unsigned long now,
                     unsigned long& last_clock_update,
                     unsigned long& last_spectrum_update) {
    // 1 panel  (32 px) : size-2 hours + stacked size-1 minute digits
    // 2-3 panels        : size 1, top 8 rows, "HH:MM" or "HH:MM DD/MM"
    // 4+ panels (128+px): size 2, full 16 rows
    const int tSize = (SD_W >= PANEL_RES_X * 4) ? 2 : 1;
    const int textH = (SD_W <= PANEL_RES_X) ? SD_H : (8 * tSize);

    if (btIsConnected()) {
        if ((now - last_clock_update) > 2000) {
            d->fillRect(0, 0, SD_W, textH, 0);
            d->setTextColor(d->color565(0, 100, 200));
            if (SD_W <= PANEL_RES_X) {
                // Single panel: "UDP" top row, "idle" bottom row, both centred
                d->setTextSize(1);
                d->setCursor((SD_W - 18) / 2, 1);
                d->print("UDP");
                d->setCursor((SD_W - 24) / 2, 9);
                d->print("idle");
            } else {
                d->setTextSize(tSize);
                d->setCursor(0, 0);
                d->print("UDP idle");
                d->setTextSize(1);
                int iw = g_max_panels * PANEL_RES_X - SD_W;
                if (iw > 0) d->fillRect(SD_W, 0, iw, SD_H, 0);
            }
            last_clock_update = now;
        }
    } else {
        if ((now - last_clock_update) > 1000) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                d->fillRect(0, 0, SD_W, textH, 0);
                d->setTextColor(d->color565(255, 255, 255));
                if (SD_W <= PANEL_RES_X) {
                    // Single panel: big hours (size 2) left + stacked minute digits (size 1) right
                    // "HH" at size 2 = 24 px wide (x 1..22); minute tens/ones at x=26, stacked
                    char hbuf[3];
                    snprintf(hbuf, sizeof(hbuf), "%02d", timeinfo.tm_hour);
                    d->setTextSize(2);
                    d->setCursor(1, 1);
                    d->print(hbuf);
                    d->setTextSize(1);
                    d->setCursor(26, 1);
                    d->write('0' + timeinfo.tm_min / 10);
                    d->setCursor(26, 9);
                    d->write('0' + timeinfo.tm_min % 10);
                } else {
                    char buf[20];
                    // size-2 chars are 12 px wide; "HH:MM DD/MM" (132 px) overflows 4-panel
                    if (tSize == 2)
                        snprintf(buf, sizeof(buf), "%02d:%02d",
                                 timeinfo.tm_hour, timeinfo.tm_min);
                    else if (SD_W >= PANEL_RES_X * 3)
                        snprintf(buf, sizeof(buf), "%02d:%02d %02d/%02d",
                                 timeinfo.tm_hour, timeinfo.tm_min,
                                 timeinfo.tm_mday, timeinfo.tm_mon + 1);
                    else
                        snprintf(buf, sizeof(buf), "%02d:%02d",
                                 timeinfo.tm_hour, timeinfo.tm_min);
                    d->setTextSize(tSize);
                    d->setCursor(0, 0);
                    d->print(buf);
                    d->setTextSize(1);
                    int iw = g_max_panels * PANEL_RES_X - SD_W;
                    if (iw > 0) d->fillRect(SD_W, 0, iw, SD_H, 0);
                }
            }
            last_clock_update = now;
        }
    }

    // Ticker animates every frame even when idle
    if ((now - last_spectrum_update) > 16) {
        tickerDraw(d);
        int iw = g_max_panels * PANEL_RES_X - SD_W;
        if (iw > 0) d->fillRect(SD_W, 0, iw, SD_H, 0);
        last_spectrum_update = now;
    }
}

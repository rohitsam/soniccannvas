#pragma once
// Spectrum analyser bar graph — full rainbow, animated hue rotation,
// vertical brightness gradient, peak-hold dots.
//
// Expects:
//   PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN defined before this header.
//   g_spectrum[SPECTRUM_BINS]  from music_player.h  (0.0-1.0 per bin).

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "music_player.h"

#define SD_W (PANEL_RES_X * PANEL_CHAIN)   // 96
#define SD_H  PANEL_RES_Y                   // 16

// Peak-hold state — one entry per frequency bin
static int   sd_peak[SPECTRUM_BINS]       = {};
static float sd_peak_hold[SPECTRUM_BINS]  = {};  // remaining hold time (seconds)
static unsigned long sd_last_ms           = 0;

// ---- colour helpers -------------------------------------------------------

// Full HSV (S=1, V=1) → RGB565
static inline uint16_t hue_to_565(float hue) {
    hue = fmodf(hue, 1.0f);
    if (hue < 0) hue += 1.0f;
    float h6 = hue * 6.0f;
    int   s  = (int)h6;
    float f  = h6 - s;
    uint8_t r, g, b;
    switch (s) {
        case 0: r=255; g=(uint8_t)(f*255);       b=0;                      break;
        case 1: r=(uint8_t)((1-f)*255); g=255;   b=0;                      break;
        case 2: r=0;   g=255;           b=(uint8_t)(f*255);                break;
        case 3: r=0;   g=(uint8_t)((1-f)*255);   b=255;                    break;
        case 4: r=(uint8_t)(f*255);     g=0;     b=255;                    break;
        default:r=255; g=0;             b=(uint8_t)((1-f)*255);            break;
    }
    return ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
}

// Scale an RGB565 colour by brightness factor (0.0-1.0)
static inline uint16_t dim565(uint16_t c, float br) {
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * br);
    uint8_t g = (uint8_t)(((c >>  5) & 0x3F) * br);
    uint8_t b = (uint8_t)(( c        & 0x1F) * br);
    return ((uint16_t)r << 11) | ((uint16_t)g << 5) | b;
}

// ---- main draw function ---------------------------------------------------

void drawSpectrum(MatrixPanel_I2S_DMA* disp) {
    unsigned long now = millis();
    float dt = (now - sd_last_ms) * 0.001f;
    if (dt > 0.1f) dt = 0.1f;   // clamp on first call / long pauses
    sd_last_ms = now;

    // Hue offset animates slowly — full rainbow cycle every ~12 seconds
    static float hue_off = 0.0f;
    hue_off = fmodf(hue_off + dt * 0.083f, 1.0f);

    for (int b = 0; b < SPECTRUM_BINS; b++) {
        float mag  = g_spectrum[b];
        int   barH = (int)(mag * SD_H + 0.5f);
        barH = constrain(barH, 0, SD_H);

        // ---- peak hold ---------------------------------------------------
        // Rise instantly, hold ~0.5 s, then fall one pixel per frame
        if (barH >= sd_peak[b]) {
            sd_peak[b]      = barH;
            sd_peak_hold[b] = 0.5f;
        } else {
            sd_peak_hold[b] -= dt;
            if (sd_peak_hold[b] <= 0.0f) {
                sd_peak_hold[b] = 0.0f;
                if (sd_peak[b] > 0) sd_peak[b]--;
            }
        }

        // ---- hue for this bar (position + slow global rotation) ----------
        float base_hue = hue_off + (float)b / SPECTRUM_BINS;

        int x0     = b * 2;          // 2px-wide bar
        int yStart = SD_H - barH;    // top row of the bar

        // ---- clear empty space above bar --------------------------------
        if (barH < SD_H) {
            disp->drawFastVLine(x0,     0, SD_H - barH, 0x0000);
            disp->drawFastVLine(x0 + 1, 0, SD_H - barH, 0x0000);
        }

        // ---- gradient bar -----------------------------------------------
        // Brightest at top (full saturation), dims to ~35% at the base.
        // Hue drifts slightly warmer going downward (+0.005 per row).
        for (int row = 0; row < barH; row++) {
            int   y  = yStart + row;
            float br = 1.0f - (float)row / (float)SD_H * 0.65f;
            uint16_t px = dim565(hue_to_565(base_hue + row * 0.005f), br);
            disp->drawPixel(x0,     y, px);
            disp->drawPixel(x0 + 1, y, px);
        }

        // ---- bright white tip (top pixel of live bar) -------------------
        if (barH > 0) {
            disp->drawPixel(x0,     yStart, 0xFFFF);
            disp->drawPixel(x0 + 1, yStart, 0xFFFF);
        }

        // ---- yellow peak-hold dot (above the live bar) ------------------
        int peakY = SD_H - sd_peak[b];
        if (sd_peak[b] > barH && peakY >= 0 && peakY < SD_H) {
            disp->drawPixel(x0,     peakY, 0xFFE0);  // yellow
            disp->drawPixel(x0 + 1, peakY, 0xFFE0);
        }
    }
}

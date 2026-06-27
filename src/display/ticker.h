#pragma once
// Scrolling text ticker — overlays the bottom 8 rows of the display.
// Call tickerSet(msg) to loop, tickerSetOnce(msg) to play once then clear.
// Auto-clears after TICKER_TIMEOUT_MS without a refresh.
//
// Emoji support: embed control chars \x01–\x08 in the message string to show an
// 8×8 bitmap glyph instead of a text character.  Each glyph occupies 9 px.
//   \x01 ♪  note      \x02 ♥  heart     \x03 ★  star      \x04 ⚡  bolt
//   \x05 😊  smile     \x06 🌙  moon      \x07 🔥  fire      \x08 🎵  double-note

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "emoji.h"   // drawEmoji(), strDisplayWidth(), EMOJI_W, EMOJI_COUNT

// SD_W / SD_H come from emoji.h (which declares extern int SD_W and #defines SD_H)

#define TICKER_Y          (SD_H - 8)   // top of 8-px text strip (row 8 on a 16-row display)
#define TICKER_SPEED      0.5f          // pixels per frame  (~30 px/s at 60 fps)
#define TICKER_TIMEOUT_MS 65000UL       // auto-clear if no refresh for 65 s

static char          g_ticker_msg[128] = {};
static float         _tick_x           = 0;
static int           _tick_px_len      = 0;
static unsigned long _tick_set_ms      = 0;
static bool          _tick_once        = false;  // clear after one pass instead of looping

void tickerSet(const char* msg) {
    strncpy(g_ticker_msg, msg, sizeof(g_ticker_msg) - 1);
    g_ticker_msg[sizeof(g_ticker_msg) - 1] = 0;
    _tick_x      = (float)SD_W;
    _tick_px_len = strDisplayWidth(g_ticker_msg);
    _tick_set_ms = millis();
    _tick_once   = false;
    Serial.printf("[ticker] \"%s\"  px_len=%d\n", g_ticker_msg, _tick_px_len);
}

// Play once: scrolls fully across the display once, then disappears automatically.
void tickerSetOnce(const char* msg) {
    tickerSet(msg);
    _tick_once = true;
}

void tickerClear() {
    g_ticker_msg[0] = 0;
    _tick_once = false;
}

void tickerDraw(MatrixPanel_I2S_DMA* d) {
    if (g_ticker_msg[0] && (millis() - _tick_set_ms > TICKER_TIMEOUT_MS)) {
        g_ticker_msg[0] = 0;
        return;
    }
    if (!g_ticker_msg[0]) return;

    _tick_x -= TICKER_SPEED;

    // Black out the ticker rows before any early-return so visualization bars
    // never bleed through on the last frame of a one-shot message.
    d->fillRect(0, TICKER_Y, SD_W, 8, 0);

    if (_tick_x < -(float)_tick_px_len) {
        if (_tick_once) {
            g_ticker_msg[0] = 0;   // one pass done — clear
            _tick_once = false;
            return;
        }
        _tick_x = (float)SD_W;    // loop
    }

    d->setTextSize(1);
    d->setTextWrap(false);
    d->setTextColor(d->color565(255, 220, 80));  // warm yellow for normal chars

    float cx = _tick_x;
    for (const char* p = g_ticker_msg; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c >= 1 && c <= EMOJI_COUNT) {
            if (cx + EMOJI_W > 0 && cx < (float)SD_W)
                drawEmoji(d, (int)cx, TICKER_Y, c);
            cx += EMOJI_W;
        } else {
            if (cx + 6 > 0 && cx < (float)SD_W) {
                d->setCursor((int16_t)cx, TICKER_Y);
                d->write(c);
            }
            cx += 6;
        }
        if (cx >= (float)SD_W) break;
    }
}

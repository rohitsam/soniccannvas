#pragma once
// 8×8 pixel-art emoji for the HUB75 ticker and display.
// Use control chars \x01–\x08 in ticker text to embed a glyph.
// drawEmoji(d, x, y, id) renders the glyph; clips to SD_W.

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

extern int SD_W;    // active display width — defined in .ino.cpp
#ifndef SD_H
#define SD_H PANEL_RES_Y
#endif

#define EMOJI_W     9   // pixel columns reserved per emoji (8 px glyph + 1 px gap)
#define EMOJI_COUNT 8

enum EmojiId : uint8_t {
    EMOJI_NOTE  = 1,  // ♪  eighth note
    EMOJI_HEART = 2,  // ♥  heart
    EMOJI_STAR  = 3,  // ★  4-point star / diamond
    EMOJI_BOLT  = 4,  // ⚡  lightning bolt
    EMOJI_SMILE = 5,  // 😊  smiley face
    EMOJI_MOON  = 6,  // 🌙  crescent moon
    EMOJI_FIRE  = 7,  // 🔥  flame
    EMOJI_NOTE2 = 8,  // 🎵  double beamed note
};

// Each entry: 8 bytes, one per row, MSB = leftmost pixel column.
static const uint8_t EMOJI_BMP[EMOJI_COUNT][8] PROGMEM = {
    // 1 — ♪ eighth note (flag at top-right, stem down, oval head bottom-left)
    {0b00000110,   // .....##.
     0b00000100,   // .....#..
     0b00000100,   // .....#..
     0b00000100,   // .....#..
     0b00000100,   // .....#..
     0b00011100,   // ...###..
     0b00111100,   // ..####..
     0b00011000},  // ...##...

    // 2 — ♥ heart
    {0b00000000,
     0b01100110,   // .##..##.
     0b11111111,   // ########
     0b11111111,   // ########
     0b01111110,   // .######.
     0b00111100,   // ..####..
     0b00011000,   // ...##...
     0b00000000},

    // 3 — ★ diamond star (4-point; clean and unmistakable at 8 px)
    {0b00010000,   // ...#....
     0b00111000,   // ..###...
     0b01111100,   // .#####..
     0b11111110,   // #######.
     0b01111100,   // .#####..
     0b00111000,   // ..###...
     0b00010000,   // ...#....
     0b00000000},

    // 4 — ⚡ lightning bolt (Z-zigzag: upper band left → kink → tip right)
    {0b00111100,   // ..####..
     0b01111000,   // .####...
     0b11111100,   // ######..
     0b00111110,   // ..#####.
     0b00011110,   // ...####.
     0b00001100,   // ....##..
     0b00000100,   // .....#..
     0b00000000},

    // 5 — 😊 smiley face
    {0b00111100,   // ..####..
     0b01000010,   // .#....#.
     0b10100101,   // #.#..#.#  border + eyes at col 2 & 5
     0b10000001,   // #......#
     0b10000001,   // #......#
     0b10100101,   // #.#..#.#  border + smile corners (high)
     0b01011010,   // .#.##.#.  border + smile bottom (low → U-curve)
     0b00111100},  // ..####..

    // 6 — 🌙 crescent moon (thin arc, open on left)
    {0b00010000,   // ...#....   top tip
     0b00011100,   // ...###..   top curve
     0b00001110,   // ....###.   right body
     0b00001110,   // ....###.
     0b00001110,   // ....###.
     0b00001110,   // ....###.
     0b00011100,   // ...###..   bottom curve
     0b00010000},  // ...#....   bottom tip

    // 7 — 🔥 flame (tip → wide base, hot core indented)
    {0b00010000,   // ...#....
     0b00111000,   // ..###...
     0b01111100,   // .#####..
     0b11111110,   // #######.
     0b11111110,   // #######.
     0b11101110,   // ###.###.
     0b01111100,   // .#####..
     0b00111000},  // ..###...

    // 8 — 🎵 double beamed eighth notes (beam at top, two heads at bottom)
    {0b01111110,   // .######.  beam
     0b01000010,   // .#....#.  stems
     0b01000010,   // .#....#.
     0b01000010,   // .#....#.
     0b11100111,   // ###..###  heads start (cols 0-2 & 5-7)
     0b11100111,   // ###..###
     0b01100110,   // .##..##.  head bottoms
     0b00000000},
};

// Default per-emoji colours (RGB565).
static const uint16_t EMOJI_COLOR[EMOJI_COUNT] = {
    0xFFFF,  // NOTE   — white
    0xF800,  // HEART  — red
    0xFFE0,  // STAR   — yellow
    0xFFE0,  // BOLT   — yellow
    0xFFE0,  // SMILE  — yellow
    0xA51F,  // MOON   — steel-blue
    0xFA80,  // FIRE   — orange  (R=31,G=20,B=0)
    0x07E0,  // NOTE2  — green
};

// Draw one emoji glyph at pixel (x, y). Clips to [0, SD_W) × [0, SD_H).
// Pass color=0 to use the default colour for that emoji.
static inline void drawEmoji(MatrixPanel_I2S_DMA* d, int x, int y,
                             uint8_t id, uint16_t color = 0) {
    if (id < 1 || id > EMOJI_COUNT) return;
    if (color == 0) color = EMOJI_COLOR[id - 1];
    for (int row = 0; row < 8; row++) {
        int py = y + row;
        if (py < 0 || py >= SD_H) continue;
        uint8_t line = pgm_read_byte(&EMOJI_BMP[id - 1][row]);
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= SD_W) continue;
            if (line & (0x80 >> col))
                d->drawPixel(px, py, color);
        }
    }
}

// Total display pixel width of a string, accounting for emoji control chars.
static inline int strDisplayWidth(const char* s) {
    int w = 0;
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        w += (c >= 1 && c <= EMOJI_COUNT) ? EMOJI_W : 6;
    }
    return w;
}

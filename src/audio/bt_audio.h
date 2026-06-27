#pragma once
// UDP audio receiver for spectrum visualization.
// ESP32-S3 has Bluetooth LE only — no Classic BT, no A2DP.
// Instead, any device with ffmpeg or a compatible app can stream raw PCM via UDP.
//
// Protocol  : raw PCM, signed 16-bit little-endian, 44100 Hz
//             mono or stereo (auto-detected from packet size)
// Port      : 4210 (UDP)
//
// Sender examples:
//   Linux/Mac : ffmpeg -f alsa  -i default            -ar 44100 -ac 1 -f s16le udp://<IP>:4210
//   Windows   : ffmpeg -f dshow -i audio="Microphone"  -ar 44100 -ac 1 -f s16le udp://<IP>:4210
//   Android   : ffmpeg -f android_audiorecord -i :0    -ar 44100 -ac 1 -f s16le udp://<IP>:4210
//
// The function names below keep the bt_* prefix so the main sketch needs no changes.

#include <Arduino.h>
#include <WiFiUDP.h>
#include "music_player.h"

#define UDP_AUDIO_PORT 4210

static WiFiUDP       udp_audio;
static volatile bool bt_streaming = false;
static volatile bool bt_connected = false;
static char          bt_peer[64]  = {};
static unsigned long udp_last_ms  = 0;
static bool          udp_first    = true;  // true until first packet of a session

// Called once in setup()
void btAudioInit() {
    udp_audio.begin(UDP_AUDIO_PORT);
    Serial.printf("[UDP] audio receiver on port %d (s16le 44100Hz mono/stereo)\n", UDP_AUDIO_PORT);
}

// Call every loop() iteration — non-blocking, drains all waiting packets
void udpAudioTick() {
    static uint8_t buf[1472];  // largest UDP payload without IP fragmentation

    int sz;
    while ((sz = udp_audio.parsePacket()) > 0) {
        int n = udp_audio.read(buf, min(sz, (int)sizeof(buf)));
        if (n < 2) continue;

        // First packet of a new session: log sender, stop any file playback
        if (!bt_connected) {
            IPAddress ip = udp_audio.remoteIP();
            snprintf(bt_peer, sizeof(bt_peer), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            Serial.printf("[UDP] stream from %s\n", bt_peer);
        }
        bt_connected = true;
        bt_streaming = true;
        udp_last_ms  = millis();

        if (udp_first) {
            udp_first = false;
            musicPlayerStop();
            fft_fill = 0; fft_do_compute = false;
            rb_head = rb_tail = 0;
        }

        // Feed samples into the shared FFT pipeline.
        // Stereo if packet length is a multiple of 4 (2 ch × 2 bytes); otherwise mono.
        const int16_t* s      = reinterpret_cast<const int16_t*>(buf);
        bool           stereo = (n % 4 == 0) && (n >= 4);
        int            frames = stereo ? (n / 4) : (n / 2);

        for (int i = 0; i < frames; i++) {
            if (fft_do_compute) break;  // FFT task still crunching last block
            int32_t mono = stereo
                ? (((int32_t)s[i * 2] + s[i * 2 + 1]) >> 1)
                : (int32_t)s[i];
            fft_real_in[fft_fill] = (float)mono / 32768.0f;
            if (++fft_fill >= FFT_N) {
                fft_fill       = 0;
                mp_sample_rate = 44100;
                fft_do_compute = true;
            }
        }
    }

    // 2-second silence → mark disconnected
    if (bt_connected && (millis() - udp_last_ms > 2000)) {
        bt_streaming = false;
        bt_connected = false;
        udp_first    = true;
        memset(bt_peer, 0, sizeof(bt_peer));
        Serial.println("[UDP] stream ended (timeout)");
    }
}

bool        btIsConnected() { return bt_connected; }
bool        btIsStreaming()  { return bt_streaming; }
const char* btPeerName()    { return bt_peer; }

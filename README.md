# SonicCanvas

Real-time music visualizer for the **Huidu HD-WF2** (ESP32-S3) HUB75 LED matrix controller. Streams audio from a PC via UDP, runs 11 visualization modes selectable from a web UI, and falls back to a clock display when idle.

---

## Hardware

| Component | Details |
|-----------|---------|
| Controller | Huidu HD-WF2 — ESP32-S3, 8 MB PSRAM, 16 MB flash |
| Display | 3 × 32×16 HUB75E panels chained → **96 × 16 pixels** total |
| Panel driver | FM6126A |
| Audio output | GPIO 14 → RC low-pass filter → speaker amp (PWM-DAC @ 312 kHz) |
| RTC | BM8563 on I2C1 (SDA=41, SCL=42) |
| Run LED | GPIO 40 (PWM fade) |
| Push button | GPIO 11 (deep-sleep trigger) |

> The WF2 has two HUB75 ports (X1, X2). Only X1 is used; X2 E-pin is unassigned.

---

## Features

- **11 real-time visualizations** driven by FFT of incoming audio
- **UDP raw PCM receiver** — stream from any device with ffmpeg
- **Web UI** — switch visualizations, manage files, trigger OTA firmware update
- **LittleFS WAV playback** — plays `/music.wav` on boot if present; LEDC PWM-DAC output
- **RTC clock fallback** — displays time when no audio is streaming
- **NTP sync** on first boot (updates BM8563 RTC if >30 days stale)
- **OTA updates** at `/update`

---

## Visualizations

| # | Name | Description |
|---|------|-------------|
| 0 | Spectrum | Rainbow bars with animated hue rotation and peak-hold dots |
| 1 | Mirror | Bars grow outward from centre, symmetric top/bottom |
| 2 | Waterfall | Scrolling spectrum history — newest row at top |
| 3 | Color Organ | Bass / mid / treble as full-width colour blocks |
| 4 | Oscilloscope | Raw waveform snapshot in rainbow colours |
| 5 | Echo Wave | Waveform with persistence trail (78% decay per frame) |
| 6 | Fire | Cellular-automaton fire, intensity driven by loudness |
| 7 | VU Meter | Three horizontal bars (bass / mid / treble) with peak hold |
| 8 | Beat Flash | Full-screen colour burst on transients, dims between beats |
| 9 | Plasma | Animated sine interference — multicolour lava-lamp effect |
| 10 | Starfield | 3-D star zoom, speed driven by loudness, per-star rainbow colours |

Switch modes at runtime from the web UI (`http://<IP>/`) or by sending `GET /viz?n=N`.

---

## Audio Streaming

The ESP32-S3 has no Bluetooth Classic hardware. Audio arrives as **raw PCM over UDP** on port **4210**.

### Windows (Stereo Mix — streams everything playing on the PC)

```bat
ffmpeg -f dshow -audio_buffer_size 50 -i audio="Stereo Mix (Realtek(R) Audio)" ^
       -ar 44100 -ac 1 -f s16le -flush_packets 1 "udp://192.168.x.x:4210?pkt_size=882"
```

Replace the IP with the one shown on the matrix at startup.

### Linux / Mac

```bash
ffmpeg -f alsa -i default -ar 44100 -ac 1 -f s16le -flush_packets 1 "udp://192.168.x.x:4210?pkt_size=882"
```

**Protocol:** signed 16-bit little-endian PCM, 44100 Hz, mono or stereo (auto-detected from packet size).  
`pkt_size=882` = 10 ms chunks for low latency. Stereo is downmixed to mono on the ESP32.

After 2 seconds of silence the stream is marked idle and the clock is shown.

> **Install ffmpeg on Windows:** `winget install Gyan.FFmpeg`  
> Enable Stereo Mix: Sound settings → Recording devices → right-click → Show disabled devices → enable Stereo Mix.

---

## Web Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Visualizer selector, UDP status, ffmpeg command |
| `/viz?n=N` | GET | Switch to visualization N (0–10) |
| `/play?f=/path.wav` | GET | Play a WAV file from LittleFS via LEDC output |
| `/play?f=/path.wav&stop=1` | GET | Stop playback |
| `/fs` | GET | File manager — list, download, delete files |
| `/fs/download?f=/path` | GET | Download a file |
| `/fs/delete?f=/path` | GET | Delete a file |
| `/update` | GET | ElegantOTA firmware update UI |

---

## Building & Flashing

### Requirements

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- ffmpeg on the streaming PC

### Configure Wi-Fi

Edit the credentials in the main sketch before building:

```cpp
// src/HD-WF1-WF2-LED-MatrixPanel-DMA.ino.cpp
const char *wifi_ssid = "your_network";
const char *wifi_pass = "your_password";
```

### Build & flash

```bash
pio run -e huidu_hd_wf2 --target upload
pio device monitor --baud 115200
```

The IP address is shown on the matrix for 3 seconds after boot.

---

## Architecture

```
loop() ──────────────────────────────────────────────────────────────────
  udpAudioTick()            drain UDP packets → fft_real_in[]
  webServer.handleClient()
  drawVisualization()       @16 ms (60 fps)
    └── switch(g_viz_mode) → viz_*()

Core 1 — fft_task (priority 1)
  ArduinoFFT<float> on 512 samples → g_spectrum[48], g_amplitude, g_osc_buf[96]

Core 0 — feeder_task (priority 3, spawned per WAV file)
  LittleFS → ring buffer → audio_cb (esp_timer ISR, IRAM_ATTR)
                            └── LEDC PWM duty update @ sample_rate Hz
```

### Audio data flow

```
UDP packet (s16le 44100 Hz)
  └─ udpAudioTick() ──→ fft_real_in[512] ──→ fft_task
                                              ├─→ g_spectrum[48]   visualizer bars
                                              ├─→ g_amplitude       fire/stars/plasma speed
                                              └─→ g_osc_buf[96]    oscilloscope/echo wave
```

### Memory layout

| Buffer | Size | Location | Reason |
|--------|------|----------|--------|
| `fft_real_in[512]` | 2 KB | DRAM | Filled by timer ISR — must be in DRAM |
| `g_osc_buf[96]` | 384 B | DRAM | |
| `_sin_tab[256]` | 1 KB | DRAM (`DRAM_ATTR`) | Plasma LUT — consistent latency |
| `wf_buf[16][96]` | 3 KB | **PSRAM** | Frees DRAM for WiFi/stack |
| `ew_buf[16][96]` | 3 KB | **PSRAM** | |
| `fire_g[18][96]` | 1.7 KB | **PSRAM** | |
| DMA display buffer | ~6 KB | DRAM | MatrixPanel library |

---

## Speed Optimizations

- **`IRAM_ATTR` on `audio_cb`** — timer ISR runs from IRAM, avoids flash cache misses
- **`float` throughout** — ESP32-S3 has hardware single-precision FPU; `double` is software emulation (~5–10× slower)
- **Sin lookup table** — 256-entry DRAM table replaces `sinf()` in plasma (6144 `sinf()` calls/frame → table lookups, ~40× faster)
- **XOR-shift PRNG** — replaces `random()` in fire and starfield (~4× faster)
- **`memmove` waterfall scroll** — one call instead of 15 `memcpy` calls
- **`drawRGBBitmap`** — batch renders waterfall and echo wave frames
- **Fire no-modulo inner loop** — edge pixels handled separately; 94 of 96 pixels/row avoid `%SD_W`
- **`-O2` build flag** — enables full compiler optimizations for user code

---

## Libraries

| Library | Purpose |
|---------|---------|
| [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) | HUB75 DMA display driver |
| [arduinoFFT](https://github.com/kosme/arduinoFFT) v2 | `ArduinoFFT<float>` spectrum analysis |
| [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) | OTA firmware update |
| [I2C BM8563 RTC](https://github.com/tanakamasayuki/I2C_BM8563) | Hardware RTC |
| [ESP32Time](https://github.com/fbiego/ESP32Time) | Software RTC helper |
| [Bounce2](https://github.com/thomasfredericks/Bounce2) | Button debounce |
| Platform | [Jason2866/platform-espressif32](https://github.com/Jason2866/platform-espressif32) Arduino/IDF53 branch (Huidu board manifests) |

---

## Hardware Links

- HD-WF1 (ESP32-S2): [AliExpress](https://www.aliexpress.com/item/1005005038544582.html)
- HD-WF2 (ESP32-S3): [AliExpress](https://www.aliexpress.com/item/1005002271988988.html)

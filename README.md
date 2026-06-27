# SonicCanvas

Real-time music visualizer for the **Huidu HD-WF2** (ESP32-S3) HUB75 LED matrix controller. Streams audio from a PC via UDP, runs 11 visualization modes selectable from a web UI, and falls back to a clock display when idle.

---

## Hardware

| Component | Details |
|-----------|---------|
| Controller | Huidu HD-WF2 — ESP32-S3, 8 MB PSRAM, 16 MB flash |
| Display | 1–6 × 32×16 HUB75E panels chained (default 3 → **96 × 16 px**); configurable at runtime |
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
- **Web UI** — switch visualizations, set panel count, manage files, trigger OTA firmware update
- **Runtime panel count** — set active visualization width (1–6 panels) and physical chain size independently; inactive panels go black; saved to NVS, applied after reboot
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

## Scrolling Lyrics (YouTube + Genius)

`tools/youtube_lyrics.py` watches your browser, detects the YouTube video title, fetches lyrics from Genius, and streams them line-by-line to the display ticker.

### Setup

```bash
pip install pygetwindow requests lyricsgenius
```

1. Go to [genius.com/api-clients](https://genius.com/api-clients) → New API Client → Generate Access Token
2. Paste the token into `GENIUS_TOKEN` at the top of `tools/youtube_lyrics.py`
3. Flash the firmware, then run:

```bash
python tools/youtube_lyrics.py
# or with explicit IP:
python tools/youtube_lyrics.py 192.168.1.42
```

### How it works

```
YouTube tab: "Blinding Lights - The Weeknd - YouTube"
    ↓  parse_title() strips noise  →  song="Blinding Lights"  artist="The Weeknd"
    ↓  Genius API fetches lyrics
    ↓  lines sent to ESP32 timed to scroll speed (one line finishes → next begins)
GET /text?msg=I've+been+running+out+of+time
    ↓
Scrolls across bottom 8 rows in warm yellow  |  visualizer runs on upper rows
```

- Falls back to scrolling the song title if Genius can't find lyrics
- Auto-clears when the YouTube tab is closed
- Skips `[Chorus]` / `[Verse]` section headers automatically

---

## Emoji in ticker messages

Any ticker message (HTTP `/text?msg=…` or `tickerSet()` in firmware) can include 8×8 pixel-art emoji by embedding the matching control character (`\x01`–`\x08`).  Each glyph occupies 9 pixels in the scroll stream.

| Control char | Glyph | Description |
|---|---|---|
| `\x01` | ♪ | eighth note (white) |
| `\x02` | ♥ | heart (red) |
| `\x03` | ★ | diamond star (yellow) |
| `\x04` | ⚡ | lightning bolt (yellow) |
| `\x05` | 😊 | smiley face (yellow) |
| `\x06` | 🌙 | crescent moon (steel-blue) |
| `\x07` | 🔥 | flame (orange) |
| `\x08` | 🎵 | double beamed note (green) |

### Python example

```python
import requests, urllib.parse

def ticker(ip, msg):
    requests.get(f"http://{ip}/text?msg={urllib.parse.quote(msg)}")

# Prefix a song title with a music note glyph
ticker("192.168.1.42", "\x01 Blinding Lights \x02")

# Show a now-playing message with fire + heart
ticker("192.168.1.42", "\x07 fire track \x02")
```

### curl example

```bash
curl "http://192.168.1.42/text?msg=%01%20Now+Playing%3A+Blinding+Lights+%02"
```

(`%01` = `\x01` note, `%02` = `\x02` heart, URL-encoded)

---

## Panel Count

Two independent panel counts are stored in NVS:

| Setting | NVS key | Default | Meaning |
|---------|---------|---------|---------|
| **Active panels** | `panels` | 3 | Visualization area width (`SD_W = active × 32`) |
| **Physical chain** | `max_panels` | = active | Total panels physically wired to the controller |

The DMA hardware always drives the full physical chain. Panels beyond the active count receive solid black, so they appear off. Font size for the idle clock scales automatically: **size 2** (16 px, full display height) for 4+ active panels; **size 1** (8 px, top half) for 1–3 active panels.

### Change from the web UI

1. Open `http://<device-IP>/`
2. Set **Active panels** — the visualization area (1–6)
3. Set **Physical chain** — how many panels are physically connected (must be ≥ active)
4. Click **Apply & Reboot** and confirm
5. The device reboots (~3 s) and the page reloads automatically

### Change via HTTP

```
GET http://<IP>/panels                → "active=N max=M"
GET http://<IP>/panels?n=2&max=3     → set 2 active panels on a 3-panel physical chain, reboot
GET http://<IP>/panels?n=4           → set 4 active (max stays unchanged), reboot
```

Rules: `n` must be 1–6, `max` must be ≥ `n` and ≤ 6.

### Notes

- Settings persist across power cycles (NVS)
- All visualizations and the ticker scale automatically to the active width
- Inactive panels go **black** every frame — they won't show stale visualization content
- The simulator also has a panel count control under **Display Size** (no reboot needed there)

---

## Web Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Visualizer selector, panel count, UDP status, ffmpeg command |
| `/viz?n=N` | GET | Switch to visualization N (0–10) |
| `/panels` | GET | Return `active=N max=M` |
| `/panels?n=N&max=M` | GET | Set active + physical panel counts, save to NVS, reboot |
| `/text?msg=...` | GET | Set scrolling ticker message (max 127 chars) |
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
40
---

## Hardware Links

- HD-WF1 (ESP32-S2): [AliExpress](https://www.aliexpress.com/item/1005005038544582.html)
- HD-WF2 (ESP32-S3): [AliExpress](https://www.aliexpress.com/item/1005002271988988.html)

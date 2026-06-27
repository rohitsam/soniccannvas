#pragma once
// WAV file player with FFT spectrum output.
//
// Hardware: connect GPIO 14 through a 1kΩ resistor + 100nF cap to ground
//           (simple RC low-pass filter) → speaker amp or headphone amp.
//           ESP32-S3 has no internal DAC; this is a PWM-DAC at ~312 kHz carrier.
//
// Usage:
//   musicPlayerInit();                 // call once in setup()
//   musicPlayerPlay("/music.wav");     // file must exist in LittleFS
//   // g_spectrum[0..SPECTRUM_BINS-1] updated automatically (0.0–1.0)

#include <Arduino.h>
#include <LittleFS.h>
#include "driver/ledc.h"
#include "esp_timer.h"
#include <arduinoFFT.h>

// ---- configuration -------------------------------------------------------
#define AUDIO_OUT_PIN       14          // free GPIO on WF2
#define AUDIO_LEDC_TIMER    LEDC_TIMER_1
#define AUDIO_LEDC_CHANNEL  LEDC_CHANNEL_1
#define AUDIO_LEDC_BITS     LEDC_TIMER_8_BIT

#define RING_BUF_SIZE  4096
#define FFT_N          512
#define SPECTRUM_BINS  48   // must equal PANEL_RES_X*PANEL_CHAIN/2 for 2px-wide bars

// ---- ring buffer (lock-free single-producer / single-consumer) -----------
static uint8_t         rb_buf[RING_BUF_SIZE];
static volatile uint32_t rb_head = 0;
static volatile uint32_t rb_tail = 0;

static inline int  rb_count()          { return (rb_head - rb_tail + RING_BUF_SIZE) % RING_BUF_SIZE; }
static inline int  rb_space()          { return RING_BUF_SIZE - 1 - rb_count(); }
static inline void rb_push(uint8_t b)  { rb_buf[rb_head] = b; rb_head = (rb_head + 1) % RING_BUF_SIZE; }
static inline uint8_t rb_pop()         { uint8_t b = rb_buf[rb_tail]; rb_tail = (rb_tail + 1) % RING_BUF_SIZE; return b; }

// ---- shared FFT input buffer ---------------------------------------------
// float (not double): ESP32-S3 FPU only accelerates single-precision
static float fft_real_in[FFT_N];       // filled by audio_cb
static int   fft_fill    = 0;
static volatile bool fft_do_compute = false;

// ---- public spectrum output ----------------------------------------------
float g_spectrum[SPECTRUM_BINS];       // 0.0–1.0, updated by fft_task

// ---- shared globals for visualizer ---------------------------------------
#define OSC_BUF_SIZE 96
float          g_amplitude = 0.0f;     // smoothed peak loudness 0-1
float          g_osc_buf[OSC_BUF_SIZE] = {};  // most-recent sample snapshot
volatile bool  g_osc_ready = false;

// ---- playback state ------------------------------------------------------
static bool     mp_playing     = false;
static File     mp_file;
static char     mp_path[64]    = {};   // current file path
static uint32_t mp_bytes_left  = 0;
static uint32_t mp_data_offset = 0;
static uint32_t mp_data_size   = 0;
static uint16_t mp_channels    = 1;
static uint16_t mp_bits        = 8;
static uint32_t mp_sample_rate = 8000;
static esp_timer_handle_t mp_timer = nullptr;

// ---- audio timer callback (fires at mp_sample_rate Hz) ------------------
// IRAM_ATTR: timer ISR must execute from IRAM, not cached flash
static void IRAM_ATTR audio_cb(void*) {
    uint8_t s = 128;
    if (rb_head != rb_tail) s = rb_pop();

    ledc_set_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CHANNEL, s);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, AUDIO_LEDC_CHANNEL);

    if (!fft_do_compute) {
        fft_real_in[fft_fill] = (float)s - 128.0f;
        if (++fft_fill >= FFT_N) {
            fft_fill      = 0;
            fft_do_compute = true;
        }
    }
}

// ---- minimal WAV header parser (PCM only) --------------------------------
static bool wav_parse(File& f, uint32_t* dataSize,
                      uint16_t* ch, uint16_t* bits, uint32_t* sr) {
    uint8_t h[44];
    if (f.read(h, 44) != 44)                               return false;
    if (memcmp(h, "RIFF", 4) || memcmp(h+8, "WAVE", 4))   return false;
    if (memcmp(h+12, "fmt ", 4))                           return false;

    uint16_t fmt = h[20] | (h[21]<<8);
    if (fmt != 1 && fmt != 3) {
        Serial.println("WAV: only PCM supported"); return false;
    }
    *ch   = h[22] | (h[23]<<8);
    *sr   = h[24] | (h[25]<<8) | (h[26]<<16) | (h[27]<<24);
    *bits = h[34] | (h[35]<<8);

    // seek to data chunk (handles LIST and other junk chunks)
    uint32_t fmtSz = h[16]|(h[17]<<8)|(h[18]<<16)|(h[19]<<24);
    f.seek(20 + fmtSz);
    uint8_t c[8];
    while (f.available() >= 8) {
        f.read(c, 8);
        uint32_t sz = c[4]|(c[5]<<8)|(c[6]<<16)|(c[7]<<24);
        if (!memcmp(c, "data", 4)) { *dataSize = sz; return true; }
        f.seek(f.position() + sz);
    }
    return false;
}

// ---- file feeder task (runs on core 0) -----------------------------------
static void feeder_task(void*) {
    uint8_t  buf[512];
    bool     stereo = (mp_channels >= 2);
    bool     bit16  = (mp_bits == 16);
    uint32_t step   = (stereo ? 2 : 1) * (bit16 ? 2 : 1);

    while (mp_playing && mp_bytes_left > 0) {
        int space = rb_space();
        if (space > 64) {
            int want = (int)min((uint32_t)sizeof(buf), mp_bytes_left);
            want     = min(want, space * (int)step);
            int n    = mp_file.read(buf, want);
            if (n <= 0) break;
            mp_bytes_left -= n;

            for (int i = 0; i + (int)step - 1 < n; i += step) {
                uint8_t s;
                if (bit16) {
                    int16_t s16 = (int16_t)(buf[i] | (buf[i+1]<<8));
                    s = (uint8_t)((s16 >> 8) + 128);
                } else {
                    s = buf[i];
                }
                rb_push(s);
            }
        } else {
            vTaskDelay(1);
        }
    }
    mp_playing = false;
    esp_timer_stop(mp_timer);
    mp_file.close();
    Serial.println("[music] playback finished");
    vTaskDelete(nullptr);
}

// ---- FFT task (runs on core 1) ------------------------------------------
static void fft_task(void*) {
    // float arrays: ESP32-S3 FPU handles float in hardware; double is software only
    // 512 floats × 2 arrays = 4 KB vs 8 KB for double — fits comfortably in stack
    static float r[FFT_N], im[FFT_N];

    while (true) {
        if (fft_do_compute) {
            memcpy(r, fft_real_in, sizeof(r));
            memcpy(g_osc_buf, r + (FFT_N - OSC_BUF_SIZE), OSC_BUF_SIZE * sizeof(float));
            g_osc_ready = true;
            memset(im, 0, sizeof(im));
            fft_do_compute = false;

            ArduinoFFT<float> fft(r, im, FFT_N, (float)mp_sample_rate);
            fft.windowing(FFTWindow::Hamming, FFTDirection::Forward);
            fft.compute(FFTDirection::Forward);
            fft.complexToMagnitude();

            float peak = 1.0f;
            for (int i = 1; i < FFT_N/2; i++)
                if (r[i] > peak) peak = r[i];

            for (int b = 0; b < SPECTRUM_BINS; b++) {
                float lo_f = (float)(b)     / SPECTRUM_BINS;
                float hi_f = (float)(b + 1) / SPECTRUM_BINS;
                int lo = 1 + (int)((FFT_N/2 - 1) * lo_f * lo_f);
                int hi = 1 + (int)((FFT_N/2 - 1) * hi_f * hi_f);
                hi = max(hi, lo + 1);
                float mx = 0;
                for (int k = lo; k < min(hi, FFT_N/2); k++)
                    if (r[k] > mx) mx = r[k];

                float norm = mx / peak;
                // smooth: 60% old, 40% new (faster attack handled by fall-through)
                g_spectrum[b] = (g_spectrum[b] > norm)
                    ? g_spectrum[b] * 0.75f + norm * 0.25f   // decay
                    : g_spectrum[b] * 0.30f + norm * 0.70f;  // attack
            }
            // Update overall loudness for amplitude-driven visualizers
            float _pk = 0;
            for (int b = 0; b < SPECTRUM_BINS; b++) if (g_spectrum[b] > _pk) _pk = g_spectrum[b];
            g_amplitude = g_amplitude * 0.7f + _pk * 0.3f;
        }
        vTaskDelay(1);
    }
}

// ---- public API ----------------------------------------------------------
void musicPlayerInit() {
    memset(g_spectrum, 0, sizeof(g_spectrum));

    ledc_timer_config_t lt = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = AUDIO_LEDC_BITS,
        .timer_num       = AUDIO_LEDC_TIMER,
        .freq_hz         = 312500,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&lt);

    ledc_channel_config_t lc = {
        .gpio_num   = AUDIO_OUT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = AUDIO_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = AUDIO_LEDC_TIMER,
        .duty       = 128,
        .hpoint     = 0,
    };
    ledc_channel_config(&lc);

    esp_timer_create_args_t ta = {};
    ta.callback              = audio_cb;
    ta.arg                   = nullptr;
    ta.name                  = "audio";
    ta.skip_unhandled_events = true;
    esp_timer_create(&ta, &mp_timer);

    xTaskCreatePinnedToCore(fft_task, "fftTask", 8192, nullptr, 1, nullptr, 1);
    Serial.println("[music] player ready – upload /music.wav to LittleFS then call musicPlayerPlay(\"/music.wav\")");
}

void musicPlayerStop() {
    mp_playing = false;
    esp_timer_stop(mp_timer);
}

// Unified play function: opens `path` (or reuses current file if same path),
// seeks to `seconds`, and starts playback. Blocks ~150 ms only when stopping.
bool musicPlayerPlayFrom(const char* path, float seconds) {
    bool needReload = (strcmp(path, mp_path) != 0 || mp_data_size == 0);

    if (mp_playing || needReload) {
        mp_playing = false;
        esp_timer_stop(mp_timer);
        if (needReload || !mp_file) {
            if (mp_file) mp_file.close();
        }
        vTaskDelay(pdMS_TO_TICKS(150)); // let feeder_task exit
        rb_head = rb_tail = 0;
        fft_fill = 0; fft_do_compute = false;
    }

    if (needReload) {
        mp_file = LittleFS.open(path, "r");
        if (!mp_file) { Serial.printf("[music] cannot open: %s\n", path); return false; }
        uint32_t dataSize = 0;
        if (!wav_parse(mp_file, &dataSize, &mp_channels, &mp_bits, &mp_sample_rate)) {
            Serial.println("[music] bad WAV (PCM 8/16-bit only)");
            mp_file.close(); return false;
        }
        strncpy(mp_path, path, sizeof(mp_path) - 1);
        mp_data_offset = mp_file.position();
        mp_data_size   = dataSize;
        Serial.printf("[music] loaded %s  %luHz %uch %ubit  %lu B\n",
                      path, (unsigned long)mp_sample_rate,
                      mp_channels, mp_bits, (unsigned long)dataSize);
    } else if (!mp_file) {
        mp_file = LittleFS.open(mp_path, "r");
        if (!mp_file) return false;
    }

    uint32_t bps    = mp_channels * (mp_bits / 8);
    uint32_t byteOff = min((uint32_t)(seconds * mp_sample_rate * bps), mp_data_size);
    mp_file.seek(mp_data_offset + byteOff);
    mp_bytes_left = mp_data_size - byteOff;
    mp_playing    = true;

    esp_timer_start_periodic(mp_timer, 1000000UL / mp_sample_rate);
    xTaskCreatePinnedToCore(feeder_task, "wavFeed", 4096, nullptr, 3, nullptr, 0);
    return true;
}

bool musicPlayerPlay(const char* path) { return musicPlayerPlayFrom(path, 0.0f); }
bool musicPlayerSeek(float seconds)    { return musicPlayerPlayFrom(mp_path, seconds); }
bool musicPlayerIsPlaying()            { return mp_playing; }
const char* musicPlayerCurrentPath()   { return mp_path; }

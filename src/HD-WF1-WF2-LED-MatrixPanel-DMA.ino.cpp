// SonicCanvas — real-time music visualizer firmware for Huidu HUB75 LED matrix controllers.
// Example shop link: https://www.aliexpress.com/item/1005005038544582.html -> WF1
// Example shop link: https://www.aliexpress.com/item/1005002271988988.html -> WF2

#if defined(WF1)
  #include "hd-wf1-esp32s2-config.h"
#elif defined(WF2)
  #include "hd-wf2-esp32s3-config.h"
#else
  #error "Please define either WF1 or WF2"
#endif

// panel_defs.h must come before any header that uses PANEL_RES_X/Y
#include "panel_defs.h"

#include <esp_err.h>
#include <esp_log.h>
#include "debug.h"
#include "littlefs_core.h"
#include <ctime>
#include "driver/ledc.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <I2C_BM8563.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ESP32Time.h>
#include <Bounce2.h>

#define fs LittleFS

/*-------------------------------- WiFi -----------------------------------*/
const char* wifi_ssid = "Samineni 4g";
const char* wifi_pass = "samineni123$";

/*------------------------------ RTC / NTP --------------------------------*/
I2C_BM8563     rtc(I2C_BM8563_DEFAULT_ADDRESS, Wire1);
const char*    ntpServer     = "0.in.pool.ntp.org";
#define CLOCK_GMT_OFFSET_SEC (5 * 3600 + 30 * 60)   // IST = UTC+5:30

/*--------------------------- Global state --------------------------------*/
// SD_W and g_max_panels defined here; declared extern in panel_defs.h
int      SD_W         = PANEL_RES_X * 3;
int      g_max_panels = 3;
uint32_t g_breathe_start = 0;
uint8_t  g_mix_r = 0, g_mix_g = 128, g_mix_b = 160;  // color-mix default (soft teal)

/*-------------------------- Pin configurations ---------------------------*/
#if defined(WF1)
HUB75_I2S_CFG::i2s_pins _pins_x1 = {
    WF1_R1_PIN, WF1_G1_PIN, WF1_B1_PIN,
    WF1_R2_PIN, WF1_G2_PIN, WF1_B2_PIN,
    WF1_A_PIN, WF1_B_PIN, WF1_C_PIN, WF1_D_PIN, WF1_E_PIN,
    WF1_LAT_PIN, WF1_OE_PIN, WF1_CLK_PIN
};
#else
HUB75_I2S_CFG::i2s_pins _pins_x1 = {
    WF2_X1_R1_PIN, WF2_X1_G1_PIN, WF2_X1_B1_PIN,
    WF2_X1_R2_PIN, WF2_X1_G2_PIN, WF2_X1_B2_PIN,
    WF2_A_PIN, WF2_B_PIN, WF2_C_PIN, WF2_D_PIN, WF2_X1_E_PIN,
    WF2_LAT_PIN, WF2_OE_PIN, WF2_CLK_PIN
};
HUB75_I2S_CFG::i2s_pins _pins_x2 = {
    WF2_X2_R1_PIN, WF2_X2_G1_PIN, WF2_X2_B1_PIN,
    WF2_X2_R2_PIN, WF2_X2_G2_PIN, WF2_X2_B2_PIN,
    WF2_A_PIN, WF2_B_PIN, WF2_C_PIN, WF2_D_PIN, WF2_X2_E_PIN,
    WF2_LAT_PIN, WF2_OE_PIN, WF2_CLK_PIN
};
#endif

/*-------------------------- Class instances ------------------------------*/
WebServer            webServer;
WiFiMulti            wifiMulti;
ESP32Time            esp32rtc;
MatrixPanel_I2S_DMA* dma_display = nullptr;
Bounce2::Button      button      = Bounce2::Button();

TaskHandle_t Task1;
TaskHandle_t Task2;

/*---------------------- Feature modules (order matters) ------------------*/
#include "led_pwm_handler.h"
#include "music_player.h"
#include "visualizer.h"
#include "bt_audio.h"
#include "emoji.h"
#include "ticker.h"
#include "clock_display.h"
#include "web_ui.h"
#include "web_routes.h"

/*---------------------------- Boot counter -------------------------------*/
RTC_DATA_ATTR int bootCount = 0;

/*------------------------- Utility functions -----------------------------*/
unsigned long getEpochTime() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return 0;
    time(&now);
    return now;
}

void print_wakeup_reason() {
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT0:    Serial.println("Wakeup: EXT0");    break;
        case ESP_SLEEP_WAKEUP_EXT1:    Serial.println("Wakeup: EXT1");    break;
        case ESP_SLEEP_WAKEUP_TIMER:   Serial.println("Wakeup: timer");   break;
        case ESP_SLEEP_WAKEUP_TOUCHPAD:Serial.println("Wakeup: touchpad");break;
        case ESP_SLEEP_WAKEUP_ULP:     Serial.println("Wakeup: ULP");     break;
        default: Serial.printf("Wakeup: cold boot (%d)\n",
                               esp_sleep_get_wakeup_cause()); break;
    }
}

/*================================= SETUP ================================*/
void setup() {
    Serial.begin(115200);

    /*-- NVS: read panel counts --*/
    {
        Preferences prefs;
        prefs.begin("soniccanvas", true);
        int saved_act = prefs.getInt("panels",     3);
        int saved_max = prefs.getInt("max_panels", saved_act);
        prefs.end();
        if (saved_act < 1 || saved_act > 6) saved_act = 3;
        if (saved_max < saved_act || saved_max > 6) saved_max = saved_act;
        SD_W         = PANEL_RES_X * saved_act;
        g_max_panels = saved_max;
        Serial.printf("[panels] active=%d  max=%d  SD_W=%d\n", saved_act, g_max_panels, SD_W);
    }

    /*-- HUB75 display init --*/
    {
        HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, g_max_panels, _pins_x1);
        mxconfig.i2sspeed        = HUB75_I2S_CFG::HZ_10M;
        mxconfig.latch_blanking  = 20;
        mxconfig.clkphase        = false;
        mxconfig.driver          = HUB75_I2S_CFG::FM6126A;
        mxconfig.double_buff     = false;
        mxconfig.min_refresh_rate = 30;
        dma_display = new MatrixPanel_I2S_DMA(mxconfig);
        dma_display->begin();
        dma_display->setBrightness8(128);
        dma_display->clearScreen();
        // RGB test flash
        dma_display->fillScreenRGB888(255, 0, 0); delay(1000);
        dma_display->fillScreenRGB888(0, 255, 0); delay(1000);
        dma_display->fillScreenRGB888(0, 0, 255); delay(1000);
        dma_display->clearScreen();
    }

    /*-- WiFi --*/
    WiFi.mode(WIFI_STA);
    wifiMulti.addAP(wifi_ssid, wifi_pass);
    Serial.print("Waiting for WiFi...");
    while (wifiMulti.run() != WL_CONNECTED) Serial.print(".");
    Serial.println(" connected");

    /*-- Boot counter / wakeup reason --*/
    ++bootCount;
    Serial.println("Boot: " + String(bootCount));
    print_wakeup_reason();

    /*-- LEDC PWM for run LED --*/
    {
        ledc_timer_config_t ledc_timer = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_13_BIT,
            .timer_num       = LEDC_TIMER_0,
            .freq_hz         = 4000,
            .clk_cfg         = LEDC_AUTO_CLK
        };
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
        ledc_channel_config_t ledc_channel = {
            .gpio_num   = RUN_LED_PIN,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = LEDC_CHANNEL_0,
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,
            .hpoint     = 0
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
        xTaskCreatePinnedToCore(ledFadeTask, "ledFadeTask", 1000, NULL, 1, &Task1, 0);
    }

    /*-- LittleFS --*/
    if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {
        Serial.println("LittleFS mount failed");
        return;
    }
    listDir(LittleFS, "/", 1);

    /*-- RTC + NTP sync --*/
    {
        Wire1.begin(BM8563_I2C_SDA, BM8563_I2C_SCL);
        rtc.begin();

        I2C_BM8563_DateTypeDef rtcDate;
        I2C_BM8563_TimeTypeDef rtcTime;
        rtc.getDate(&rtcDate);
        rtc.getTime(&rtcTime);

        std::tm  curr_rtc_tm = make_tm(rtcDate.year, rtcDate.month, rtcDate.date);
        time_t   curr_rtc_ts = std::mktime(&curr_rtc_tm);
        time_t   ntp_last_ts = 0;   // persisting to file is not yet implemented

        // Sync from NTP on first cold boot; deep-sleep wakes (bootCount > 1) use RTC
        if (std::abs((long)(curr_rtc_ts - ntp_last_ts)) > (60*60*24*30) && bootCount == 1) {
            Serial.println("Syncing RTC from NTP...");
            configTime(CLOCK_GMT_OFFSET_SEC, 0, ntpServer);
            struct tm timeInfo;
            if (getLocalTime(&timeInfo)) {
                I2C_BM8563_TimeTypeDef ts;
                ts.hours   = timeInfo.tm_hour;
                ts.minutes = timeInfo.tm_min;
                ts.seconds = timeInfo.tm_sec;
                rtc.setTime(&ts);
                I2C_BM8563_DateTypeDef ds;
                ds.weekDay = timeInfo.tm_wday;
                ds.month   = timeInfo.tm_mon + 1;
                ds.date    = timeInfo.tm_mday;
                ds.year    = timeInfo.tm_year + 1900;
                rtc.setDate(&ds);
                Serial.println("RTC updated from NTP");
            }
        } else {
            esp32rtc.setTime(rtcTime.seconds, rtcTime.minutes, rtcTime.hours,
                             rtcDate.date, rtcDate.month, rtcDate.year);
            Serial.println("ESP32 RTC set from hardware: " +
                           esp32rtc.getTime("%A, %B %d %Y %H:%M:%S"));
        }
    }

    /*-- Web server routes --*/
    registerWebRoutes();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    dma_display->clearScreen();
    dma_display->setCursor(0, 0);
    dma_display->print(WiFi.localIP());
    delay(3000);
    dma_display->clearScreen();

    /*-- Audio subsystems --*/
    musicPlayerInit();
    if (LittleFS.exists("/music.wav"))
        musicPlayerPlay("/music.wav");

    btAudioInit();
    dma_display->clearScreen();
    dma_display->setCursor(0, 0);
    dma_display->setTextColor(dma_display->color565(0, 200, 80));
    dma_display->print("UDP:4210");
    delay(1500);
    dma_display->clearScreen();
}

/*================================= LOOP =================================*/
static unsigned long last_clock_update    = 0;
static unsigned long last_spectrum_update = 0;

void loop() {
    udpAudioTick();
    webServer.handleClient();

    unsigned long now = millis();

    if (musicPlayerIsPlaying() || btIsStreaming() ||
        g_viz_mode == VIZ_BREATHE || g_viz_mode == VIZ_COLORMIX) {
        if ((now - last_spectrum_update) > 16) {
            drawVisualization(dma_display);
            tickerDraw(dma_display);
            int iw = g_max_panels * PANEL_RES_X - SD_W;
            if (iw > 0) dma_display->fillRect(SD_W, 0, iw, SD_H, 0);
            last_spectrum_update = now;
        }
    } else {
        drawIdleDisplay(dma_display, now, last_clock_update, last_spectrum_update);
    }
}

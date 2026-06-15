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


#include <esp_err.h>
#include <esp_log.h>
#include "debug.h"
#include "littlefs_core.h"
#include <ctime>
#include "driver/ledc.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>

#include <WebServer.h>
#include <ESPmDNS.h>
#include <I2C_BM8563.h>   // https://github.com/tanakamasayuki/I2C_BM8563

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ElegantOTA.h> // upload firmware by going to http://<ipaddress>/update

#include <ESP32Time.h>
#include <Bounce2.h>

#define fs LittleFS

/*----------------------------- Wifi Configuration -------------------------------*/

const char *wifi_ssid = "Samineni 4g";
const char *wifi_pass = "samineni123$";

/*----------------------------- RTC and NTP -------------------------------*/

I2C_BM8563 rtc(I2C_BM8563_DEFAULT_ADDRESS, Wire1);
const char* ntpServer         = "0.in.pool.ntp.org";
const char* ntpLastUpdate     = "/ntp_last_update.txt";

// NTP Clock Offset / Timezone
#define CLOCK_GMT_OFFSET 1

/*-------------------------- HUB75E DMA Setup -----------------------------*/
#define PANEL_RES_X 32      // Number of pixels wide of each INDIVIDUAL panel module. 
#define PANEL_RES_Y 16     // Number of pixels tall of each INDIVIDUAL panel module.
#define PANEL_CHAIN 3      // Total number of panels chained one to another


#if defined(WF1)

HUB75_I2S_CFG::i2s_pins _pins_x1 = {WF1_R1_PIN, WF1_G1_PIN, WF1_B1_PIN, WF1_R2_PIN, WF1_G2_PIN, WF1_B2_PIN, WF1_A_PIN, WF1_B_PIN, WF1_C_PIN, WF1_D_PIN, WF1_E_PIN, WF1_LAT_PIN, WF1_OE_PIN, WF1_CLK_PIN};

#else

HUB75_I2S_CFG::i2s_pins _pins_x1 = {WF2_X1_R1_PIN, WF2_X1_G1_PIN, WF2_X1_B1_PIN, WF2_X1_R2_PIN, WF2_X1_G2_PIN, WF2_X1_B2_PIN, WF2_A_PIN, WF2_B_PIN, WF2_C_PIN, WF2_D_PIN, WF2_X1_E_PIN, WF2_LAT_PIN, WF2_OE_PIN, WF2_CLK_PIN};
HUB75_I2S_CFG::i2s_pins _pins_x2 = {WF2_X2_R1_PIN, WF2_X2_G1_PIN, WF2_X2_B1_PIN, WF2_X2_R2_PIN, WF2_X2_G2_PIN, WF2_X2_B2_PIN, WF2_A_PIN, WF2_B_PIN, WF2_C_PIN, WF2_D_PIN, WF2_X2_E_PIN, WF2_LAT_PIN, WF2_OE_PIN, WF2_CLK_PIN};

#endif


/*-------------------------- Class Instances ------------------------------*/
// Routing in the root page and webcamview.html natively uses the request
// handlers of the ESP32 WebServer class, so it explicitly instantiates the
// ESP32 WebServer.
WebServer           webServer;
WiFiMulti           wifiMulti;
ESP32Time           esp32rtc;  // offset in seconds GMT+1
MatrixPanel_I2S_DMA *dma_display = nullptr;

// INSTANTIATE A Button OBJECT FROM THE Bounce2 NAMESPACE
Bounce2::Button button = Bounce2::Button();

// ROS Task management
TaskHandle_t Task1;
TaskHandle_t Task2;

#include "led_pwm_handler.h"
#include "music_player.h"
#include "visualizer.h"
#include "bt_audio.h"

RTC_DATA_ATTR int bootCount = 0;

volatile bool buttonPressed = false;

IRAM_ATTR void toggleButtonPressed() {
  // This function will be called when the interrupt occurs on pin PUSH_BUTTON_PIN
  buttonPressed = true;
  ESP_LOGI("toggleButtonPressed", "Interrupt Triggered.");

   esp_deep_sleep_start();      // Sleep for e.g. 30 minutes
  // Do something here
}



/*
Method to print the reason by which ESP32
has been awaken from sleep
*/
void print_wakeup_reason(){
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch(wakeup_reason)
  {
    case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER : Serial.println("Wakeup caused by timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup caused by ULP program"); break;
    default : Serial.printf("Wakeup was not caused by deep sleep: %d\n",wakeup_reason); break;
  }
}


// Function that gets current epoch time
unsigned long getEpochTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    //Serial.println("Failed to obtain time");
    return(0);
  }
  time(&now);
  return now;
}

//
// Arduino Setup Task
//
void setup() {

  // Init Serial
  // if ARDUINO_USB_CDC_ON_BOOT is defined then the debug will go out via the USB port
  Serial.begin(115200);

  /*-------------------- START THE HUB75E DISPLAY --------------------*/
    
    // Module configuration
    HUB75_I2S_CFG mxconfig(
      PANEL_RES_X,   // module width
      PANEL_RES_Y,   // module height
      PANEL_CHAIN,   // Chain length
      _pins_x1       // pin mapping for port X1
    );
    mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_10M;  
    mxconfig.latch_blanking = 20;
    mxconfig.clkphase = false;
    mxconfig.driver = HUB75_I2S_CFG::FM6126A;
    mxconfig.double_buff = false;  
    mxconfig.min_refresh_rate = 30;


    // Display Setup
    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    dma_display->begin();
    dma_display->setBrightness8(128); //0-255
    dma_display->clearScreen();

    dma_display->fillScreenRGB888(255,0,0);
    delay(1000);
    dma_display->fillScreenRGB888(0,255,0);
    delay(1000);    
    dma_display->fillScreenRGB888(0,0,255);
    delay(1000);       
    dma_display->clearScreen();
    // dma_display->setCursor(3,4);
    // dma_display->setTextColor(dma_display->color565(255, 0, 255)); // Yellow color
    // dma_display->print("Welcome to Rohith's Home");     
    // dma_display->
    // while(1);

  /*-------------------- START THE NETWORKING --------------------*/
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(wifi_ssid, wifi_pass); // configure in the *-config.h file

  // wait for WiFi connection
  Serial.print("Waiting for WiFi to connect...");
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
  }
  Serial.println(" connected");
    

  /*-------------------- --------------- --------------------*/
  //Increment boot number and print it every reboot
  ++bootCount;
  Serial.println("Boot number: " + String(bootCount));

  //Print the wakeup reason for ESP32
  print_wakeup_reason();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();    

  // if ( wakeup_reason == ESP_SLEEP_WAKEUP_EXT0)
  // {
  //   dma_display->setCursor(3,6);
  //   dma_display->print("Wake up!");
  //   delay(1000);
  // }
  // else
  // {
  //   dma_display->print("Starting.");

  // }


  /*
    We set our ESP32 to wake up for an external trigger.
    There are two types for ESP32, ext0 and ext1 .
  */
  // esp_sleep_enable_ext0_wakeup((gpio_num_t)PUSH_BUTTON_PIN, 0); //1 = High, 0 = Low  

  /*-------------------- --------------- --------------------*/
  // BUTTON SETUP 
  // button.attach( PUSH_BUTTON_PIN, INPUT ); // USE EXTERNAL PULL-UP
  // button.interval(5);   // DEBOUNCE INTERVAL IN MILLISECONDS
  // button.setPressedState(LOW); // INDICATE THAT THE LOW STATE CORRESPONDS TO PHYSICALLY PRESSING THE BUTTON


  /*-------------------- LEDC Controller --------------------*/
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_13_BIT ,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 4000,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = RUN_LED_PIN,
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LEDC_TIMER_0,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));  


    // Start fading that LED
    xTaskCreatePinnedToCore(
      ledFadeTask,            /* Task function. */
      "ledFadeTask",                 /* name of task. */
      1000,                    /* Stack size of task */
      NULL,                     /* parameter of the task */
      1,                        /* priority of the task */
      &Task1,                   /* Task handle to keep track of created task */
      0);                       /* Core */   
    

  /*-------------------- INIT LITTLE FS --------------------*/
  if(!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)){
      Serial.println("LittleFS Mount Failed");
      return;
  }
  listDir(LittleFS, "/", 1);    
 
  /*-------------------- --------------- --------------------*/
  // Init I2C for RTC
  Wire1.begin(BM8563_I2C_SDA, BM8563_I2C_SCL);
  rtc.begin();

  // Get RTC date and time
  I2C_BM8563_DateTypeDef rtcDate;
  I2C_BM8563_TimeTypeDef rtcTime;
  rtc.getDate(&rtcDate);
  rtc.getTime(&rtcTime);
  
  time_t ntp_last_update_ts = 0;
  

  // Current RTC
  std::tm curr_rtc_tm = make_tm(rtcDate.year, rtcDate.month, rtcDate.date);    // April 2nd, 2012
  time_t  curr_rtc_ts = std::mktime(&curr_rtc_tm);

  if ( std::abs( (long int) (curr_rtc_ts - ntp_last_update_ts)) > (60*60*24*30) && (bootCount == 0))
  {
      ESP_LOGI("need_ntp_update", "Longer than 30 days since last NTP update. Performing check.");    
  
      Serial.println("Updating RTC from Internet NTP.");  

      // Set ntp time to local
      configTime(CLOCK_GMT_OFFSET * 3600, 0, ntpServer);

      // Get local time
      struct tm timeInfo;
      if (getLocalTime(&timeInfo)) {
        // Set RTC time
        I2C_BM8563_TimeTypeDef timeStruct;
        timeStruct.hours   = timeInfo.tm_hour;
        timeStruct.minutes = timeInfo.tm_min;
        timeStruct.seconds = timeInfo.tm_sec;
        rtc.setTime(&timeStruct);

        // Set RTC Date
        I2C_BM8563_DateTypeDef dateStruct;
        dateStruct.weekDay = timeInfo.tm_wday;
        dateStruct.month   = timeInfo.tm_mon + 1;
        dateStruct.date    = timeInfo.tm_mday;
        dateStruct.year    = timeInfo.tm_year + 1900;
        rtc.setDate(&dateStruct);
    }

      ntp_last_update_ts = getEpochTime();

  } else {

    esp32rtc.setTime(rtcTime.seconds, rtcTime.minutes, rtcTime.hours, rtcDate.date, rtcDate.month, rtcDate.year);  // 17th Jan 2021 15:24:30
    Serial.println("Have a valid year on the external RTC. Updating ESP32 RTC to:");    
    Serial.println(esp32rtc.getTime("%A, %B %d %Y %H:%M:%S"));   // (String) returns time with specified format 
  }

   /*-------------------- --------------- --------------------*/

    // ---- root: visualizer control + UDP streaming info ----
    webServer.on("/", []() {
      String html =
        F("<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:520px;margin:20px auto'>"
          "<h2>SonicCanvas</h2>"
          "<p><b>Visualizer:</b> <span id='vn' style='color:#0a7'></span></p>"
          "<div id='vmd' style='display:flex;flex-wrap:wrap;gap:5px;margin-bottom:14px'></div>");
      // UDP status banner
      html += F("<p style='background:#111;color:#fff;padding:6px;border-radius:4px'>&#127911; UDP audio: ");
      if (btIsConnected()) {
        html += "<span style='color:#4f4'>&#9679; Streaming from ";
        html += bt_peer;
        html += "</span>";
      } else {
        html += F("<span style='color:#fa0'>&#9679; Idle — send raw PCM to port 4210</span>");
      }
      html += F("</p>"
                "<p style='font-size:.8em;color:#555;margin-top:0'>Stream command:<br>"
                "<code style='background:#eee;padding:2px 4px'>"
                "ffmpeg -f dshow -audio_buffer_size 50 -i audio=\"Stereo Mix\" "
                "-ar 44100 -ac 1 -f s16le -flush_packets 1 \"udp://");
      html += WiFi.localIP().toString();
      html += F(":4210?pkt_size=882\"</code></p>"
                "<p><a href='/fs'>&#128193; Files</a> &nbsp; <a href='/update'>&#8593; OTA</a></p>"
                "<script>");
      html += "var curViz=" + String(g_viz_mode) + ";";
      html +=
        F("var vnm=['Spectrum','Mirror','Waterfall','Color Organ','Oscilloscope','Echo Wave','Fire','VU Meter','Beat Flash','Plasma','Starfield'];"
          "function setViz(n){"
          "  fetch('/viz?n='+n).then(function(){"
          "    curViz=n;"
          "    document.getElementById('vn').textContent=vnm[n];"
          "    document.querySelectorAll('.vb').forEach(function(b,i){"
          "      b.style.background=i===n?'#0a7':'#444';"
          "    });"
          "  });"
          "}"
          "(function(){"
          "  var vd=document.getElementById('vmd');"
          "  vnm.forEach(function(nm,i){"
          "    var b=document.createElement('button');b.textContent=nm;b.className='vb';"
          "    b.style.cssText='padding:5px 10px;border:none;border-radius:4px;cursor:pointer;"
                               "color:#fff;font-size:.82em;background:'+(i===curViz?'#0a7':'#444');"
          "    b.onclick=function(){setViz(i);};"
          "    vd.appendChild(b);"
          "  });"
          "  document.getElementById('vn').textContent=vnm[curViz];"
          "})();"
          "</script></body></html>");
      webServer.send(200, "text/html", html);
    });

    // ---- viz: set visualisation mode ----
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

    // ---- /play: trigger ESP32-side LEDC playback from LittleFS ----
    webServer.on("/play", HTTP_GET, []() {
      String path = webServer.arg("f");
      if (!path.length()) { webServer.send(400, "text/plain", "missing ?f="); return; }
      if (webServer.arg("stop") == "1") { musicPlayerStop(); webServer.send(200, "text/plain", "stopped"); return; }
      bool ok = musicPlayerPlay(path.c_str());
      webServer.send(200, "text/plain", ok ? "playing" : "error");
    });

    // ---- /fs: filesystem manager ----
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
          String size = String(entry.size()) + " B";
          html += "<tr><td>" + name + "</td><td>" + size + "</td><td>"
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

    // ---- /fs/download ----
    webServer.on("/fs/download", HTTP_GET, []() {
      String path = webServer.arg("f");
      File f = LittleFS.open(path, "r");
      if (!f) { webServer.send(404, "text/plain", "Not found"); return; }
      String filename = path.substring(path.lastIndexOf('/') + 1);
      webServer.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
      webServer.streamFile(f, "application/octet-stream");
      f.close();
    });

    // ---- /fs/delete ----
    webServer.on("/fs/delete", HTTP_GET, []() {
      String path = webServer.arg("f");
      if (path.length() > 1 && LittleFS.exists(path)) {
        LittleFS.remove(path);
        if (path == "/music.wav") { musicPlayerStop(); }
        Serial.printf("[fs] deleted %s\n", path.c_str());
      }
      webServer.sendHeader("Location", "/fs");
      webServer.send(302, "text/plain", "");
    });

    ElegantOTA.begin(&webServer);
    webServer.begin();
    Serial.println("HTTP server started");

    /*-------------------- --------------- --------------------*/
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());  

    delay(1000);

    dma_display->clearScreen();
    dma_display->setCursor(0, 0);
    dma_display->print(WiFi.localIP());
    delay(3000);
    dma_display->clearScreen();

    /*-------------------- Music Player --------------------*/
    musicPlayerInit();
    if (LittleFS.exists("/music.wav"))
        musicPlayerPlay("/music.wav");

    /*-------------------- UDP Audio Receiver --------------------*/
    btAudioInit();
    dma_display->clearScreen();
    dma_display->setCursor(0, 0);
    dma_display->setTextColor(dma_display->color565(0, 200, 80));
    dma_display->print("UDP:4210");
    delay(1500);
    dma_display->clearScreen();

}

unsigned long last_clock_update   = 0;
unsigned long last_spectrum_update = 0;
char buffer[64];

void loop()
{
    udpAudioTick();
    webServer.handleClient();

    if (musicPlayerIsPlaying() || btIsStreaming()) {
        if ((millis() - last_spectrum_update) > 16) {
            drawVisualization(dma_display);
            last_spectrum_update = millis();
        }
    } else if (btIsConnected()) {
        if ((millis() - last_clock_update) > 2000) {
            dma_display->clearScreen();
            dma_display->setCursor(0, 4);
            dma_display->setTextColor(dma_display->color565(0, 100, 200));
            dma_display->print("UDP idle");
            last_clock_update = millis();
        }
    } else {
        // clock fallback – update once per second
        if ((millis() - last_clock_update) > 1000) {
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                memset(buffer, 0, sizeof(buffer));
                snprintf(buffer, sizeof(buffer), "%02d:%02d %02d/%02d",
                         timeinfo.tm_hour, timeinfo.tm_min,
                         timeinfo.tm_mday, timeinfo.tm_mon + 1);
                dma_display->clearScreen();
                dma_display->setCursor(0, 5);
                dma_display->print(buffer);
            }
            last_clock_update = millis();
        }
    }


} // loop()
/*Using LVGL with Arduino requires some extra steps:
 *Be sure to read the docs here: https://docs.lvgl.io/master/get-started/platforms/arduino.html  */

#include "Display_ST77916.h"
#include "RTC_PCF85063.h"
#include "LVGL_Driver.h"
#include "MIC_MSM.h"
#include "SD_Card.h"
#include "GranVoice_UI.h"
#include "GranVoice_Audio.h"
#include "GeminiLive.h"
#include "BAT_Driver.h"
#include "WiFi_Manager.h"
#include <Preferences.h>

WiFiManager wifiManager;
bool wifiConnected = false;

void GeminiLoopTask(void *parameter) {
  for (;;) {
    geminiLive.loop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Background housekeeping only - WiFi/Gemini bring-up happens synchronously in
// setup(), before MIC_Init()/GranVoice_Audio_Init() claim their I2S DMA buffers
// and stream buffer. Doing it after those (as the original demo did for its own
// WiFi test) starves the WiFi driver of internal DRAM for its RX buffers
// ("Expected to init 4 rx buffer, actual is 0" -> scan fails with -2).
void Driver_Loop(void *parameter)
{
  unsigned long lastWifiRetry = 0;
  while(1)
  {
    PCF85063_Loop();
    BAT_Get_Volts();

    // WiFi watchdog: if the router reboots or the device is out of range for a
    // while, silently retry known credentials every 30s so it heals itself
    // without anyone having to touch the screen.
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry > 30000) {
      lastWifiRetry = millis();
      Serial.println("[WiFiRetry] disconnected - retrying known networks");
      if (wifiManager.retryKnownNetworks()) {
        wifiConnected = true;
        geminiLive.reconnect(); // socket belonged to the dead connection
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
void Driver_Init()
{
  Flash_test();
  BAT_Init();
  I2C_Init();
  TCA9554PWR_Init(0x00);
  Backlight_Init();
  // Keep the panel dark until LVGL has painted its first frame. Backlight_Init()
  // lights it immediately, which shows whatever noise is left in the display's
  // RAM as colourful stripes for the second or two before the UI appears.
  Set_Backlight(0);
  PCF85063_Init();
}
void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== SETUP STARTING ===");

  Serial.println("Calling Driver_Init...");
  Driver_Init();
  Serial.println("Driver_Init complete");

  Serial.println("Calling SD_Init...");
  SD_Init();
  Serial.println("SD_Init complete");

  Serial.println("Calling LCD_Init...");
  LCD_Init();
  Serial.println("LCD_Init complete");

  Serial.println("Calling Lvgl_Init...");
  Lvgl_Init();
  Serial.println("Lvgl_Init complete");

  Serial.println("Calling GranVoice_UI_Init...");
  GranVoice_UI_Init();
  Serial.println("GranVoice_UI_Init complete");

  // First frame is ready - safe to light the panel now (see Set_Backlight(0) above),
  // at whatever brightness was last chosen in settings.
  lv_refr_now(NULL);
  {
    Preferences p;
    p.begin("granvoice", true);
    LCD_Backlight = (uint8_t)p.getInt("backlight", LCD_Backlight);
    p.end();
  }
  Set_Backlight(LCD_Backlight);

  // WiFi (and Gemini) before the audio subsystem, while internal DRAM is still
  // plentiful - see the note on Driver_Loop above.
  Serial.println("Starting WiFi Manager...");
  wifiConnected = wifiManager.connectToBestNetwork();
  Serial.printf("=== WiFi connection result: %s ===\n", wifiConnected ? "SUCCESS" : "FAILED");

  if (!wifiConnected) {
    // Nothing we know about worked - let the user pick a network on-screen
    // instead of leaving them stuck with no way in short of a reflash.
    Serial.println("WiFi failed - opening on-screen network setup");
    GranVoice_UI_ShowWifiSetup();
  }

  if (wifiConnected) {
    Serial.println("Connecting to Gemini Live...");
    geminiLive.begin();
    // geminiLive.begin() only starts the connection - the actual TLS handshake
    // happens inside geminiLive.loop(). Pump it synchronously here so the
    // handshake gets first claim on heap, before MIC_Init()/GranVoice_Audio_Init()
    // below allocate their own I2S/stream buffers.
    unsigned long waitStart = millis();
    while (!geminiLive.isReady() && millis() - waitStart < 10000) {
      geminiLive.loop();
      delay(10);
    }
    Serial.printf("Gemini ready before audio init: %s\n", geminiLive.isReady() ? "yes" : "no (will keep retrying in background)");
    xTaskCreatePinnedToCore(GeminiLoopTask, "GeminiLoop", 8192, NULL, 4, NULL, 1);
  }

  Serial.println("Calling MIC_Init...");
  MIC_Init();
  Serial.println("MIC_Init complete");

  Serial.println("Calling GranVoice_Audio_Init...");
  GranVoice_Audio_Init();
  Serial.println("GranVoice_Audio_Init complete");

  Serial.println("Creating Driver_Loop task...");
  BaseType_t taskResult = xTaskCreatePinnedToCore(
    Driver_Loop,
    "DriverTask",
    4096, // needs headroom for the WiFi retry path (WiFi.begin + String work)
    NULL,
    3,
    NULL,
    0
  );

  if (taskResult == pdPASS) {
    Serial.println("Driver_Loop task created successfully");
  } else {
    Serial.println("ERROR: Failed to create Driver_Loop task!");
  }

  Serial.println("=== SETUP COMPLETE ===");
}
void loop() {
  Lvgl_Loop();
  vTaskDelay(pdMS_TO_TICKS(5));

}

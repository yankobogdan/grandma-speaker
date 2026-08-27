#include "GranVoice_UI.h"
#include "GranVoice_Audio.h"
#include "GeminiLive.h"
#include "LVGL_Driver.h"
#include "WiFi_Manager.h"
#include "BAT_Driver.h"
#include <WiFi.h>
#include <Preferences.h>

// ESP32-S3 BOOT button. Usable as a normal input once booted; pressed reads LOW.
#define BOOT_BUTTON_PIN 0

extern WiFiManager wifiManager;

// Ukrainian-capable font (LVGL's built-in Montserrat fonts are Latin-only).
// Generated from Arial Unicode.ttf, ASCII + full Cyrillic block (U+0400-U+04FF),
// with lv_font_montserrat_16 as fallback so the LV_SYMBOL_* icons (FontAwesome
// glyphs in the U+F000 private-use range, absent from the ranges above) still
// render instead of coming out as garbage boxes.
// NOTE: must be declared const to match the generated definition.
extern "C" const lv_font_t lv_font_ua_22;

enum class GVState { IDLE, LISTENING, SPEAKING };

static volatile GVState state = GVState::IDLE;
static volatile bool turnCompleteFlag = false;
static volatile bool interruptedFlag = false;
static volatile bool audioArrivedFlag = false;
static unsigned long listenStartMs = 0;

// Reply/heard text is produced on the WS task but may only be rendered on the
// LVGL task, so it lands in these buffers and GranVoice_Tick paints them.
static char pendingReply[512] = {0};
static volatile bool replyDirty = false;
static portMUX_TYPE replyMux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t* mainScreen = nullptr;
static lv_obj_t* talkBtn = nullptr;
static lv_obj_t* talkLabel = nullptr;
static lv_obj_t* replyBox = nullptr;   // scrollable container for long replies
static lv_obj_t* replyLabel = nullptr;
static lv_obj_t* wifiLabel = nullptr;
static lv_obj_t* batteryLabel = nullptr;

// Talk trigger: on-screen button, or the physical BOOT button. With the physical
// button the on-screen one shrinks to a status dot, freeing the screen for text.
static int useBootButton = -1; // -1 = not yet loaded from NVS

static bool GetUseBootButton() {
  if (useBootButton < 0) {
    Preferences p;
    p.begin("granvoice", true);
    useBootButton = p.getBool("bootbtn", false) ? 1 : 0;
    p.end();
  }
  return useBootButton == 1;
}

static void ApplyMainLayout();

static void SetUseBootButton(bool on) {
  useBootButton = on ? 1 : 0;
  Preferences p;
  p.begin("granvoice", false);
  p.putBool("bootbtn", on);
  p.end();
  ApplyMainLayout();
}

static lv_obj_t* settingsScreen = nullptr;
static lv_obj_t* wifiListScreen = nullptr;
static lv_obj_t* passwordScreen = nullptr;
static lv_obj_t* passwordTA = nullptr;
static lv_obj_t* passwordTitle = nullptr;
static char pendingSSID[64] = {0};

static void ShowSettings();
static void ShowWifiList();
static void ShowMain();

// ---------------------------------------------------------------- main screen

static bool GetUseBootButton();

static void SetVisual(GVState s) {
  // With the physical button the on-screen circle is only 92px, so it gets terse
  // labels; the roomy touch-mode button keeps the full instruction.
  const bool compact = GetUseBootButton();
  switch (s) {
    case GVState::IDLE:
      lv_obj_set_style_bg_color(talkBtn, lv_palette_main(LV_PALETTE_TEAL), 0);
      lv_label_set_text(talkLabel, compact ? "Готово" : "Натисніть,\nщоб поговорити");
      break;
    case GVState::LISTENING:
      lv_obj_set_style_bg_color(talkBtn, lv_palette_main(LV_PALETTE_RED), 0);
      lv_label_set_text(talkLabel, compact ? "Слухаю" : "Слухаю...");
      break;
    case GVState::SPEAKING:
      lv_obj_set_style_bg_color(talkBtn, lv_palette_main(LV_PALETTE_GREEN), 0);
      lv_label_set_text(talkLabel, compact ? "Говорю" : "Говорю...");
      break;
  }
}

// Single-cell Li-ion maps ~4.15V full to ~3.35V empty. The curve is flat in the
// middle, so this piecewise approximation tracks it far better than a straight
// linear voltage->percent mapping would.
static int BatteryPercent(float v) {
  struct { float v; int pct; } curve[] = {
    {4.15f, 100}, {4.00f, 85}, {3.90f, 70}, {3.80f, 55},
    {3.70f, 40},  {3.60f, 25}, {3.50f, 12}, {3.35f, 0},
  };
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (v >= curve[0].v) return 100;
  if (v <= curve[n-1].v) return 0;
  for (int i = 0; i < n - 1; i++) {
    if (v <= curve[i].v && v > curve[i+1].v) {
      float span = curve[i].v - curve[i+1].v;
      float frac = (v - curve[i+1].v) / span;
      return curve[i+1].pct + (int)(frac * (curve[i].pct - curve[i+1].pct));
    }
  }
  return 0;
}

static void UpdateBatteryStatus() {
  if (!batteryLabel) return;
  int pct = BatteryPercent(BAT_analogVolts);

  const char* icon = LV_SYMBOL_BATTERY_EMPTY;
  if (pct >= 90)      icon = LV_SYMBOL_BATTERY_FULL;
  else if (pct >= 65) icon = LV_SYMBOL_BATTERY_3;
  else if (pct >= 40) icon = LV_SYMBOL_BATTERY_2;
  else if (pct >= 15) icon = LV_SYMBOL_BATTERY_1;

  static char buf[32];
  snprintf(buf, sizeof(buf), "%s %d%%", icon, pct);
  lv_label_set_text(batteryLabel, buf);

  lv_color_t col = (pct <= 15) ? lv_palette_main(LV_PALETTE_RED)
                 : (pct <= 35) ? lv_palette_main(LV_PALETTE_ORANGE)
                               : lv_color_white();
  lv_obj_set_style_text_color(batteryLabel, col, 0);
}

static void UpdateWifiStatus() {
  if (!wifiLabel) return;
  // Quota refusal outranks the WiFi indicator: the network is fine, but nothing
  // will work, and "connected" alone would be misleading.
  if (geminiLive.isQuotaExhausted()) {
    lv_label_set_text(wifiLabel, LV_SYMBOL_WARNING " Ліміт Gemini вичерпано");
    lv_obj_set_style_text_color(wifiLabel, lv_palette_main(LV_PALETTE_RED), 0);
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    static char buf[80];
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", WiFi.SSID().c_str());
    lv_label_set_text(wifiLabel, buf);
    lv_obj_set_style_text_color(wifiLabel, lv_palette_main(LV_PALETTE_GREEN), 0);
  } else {
    lv_label_set_text(wifiLabel, LV_SYMBOL_WARNING " Немає WiFi");
    lv_obj_set_style_text_color(wifiLabel, lv_palette_main(LV_PALETTE_ORANGE), 0);
  }
}

static void CancelToIdle(const char* reason) {
  Serial.printf("[GranVoice] -> IDLE (%s)\n", reason);
  GranVoice_Audio_StopCapture();
  geminiLive.sendAudioStreamEnd();
  GranVoice_Audio_FlushPlayback();
  turnCompleteFlag = false;
  state = GVState::IDLE;
  SetVisual(GVState::IDLE);
}

static void OnButtonClicked(lv_event_t* e) {
  Serial.println("[GranVoice] button tapped");
  GranVoice_Audio_PlayTap();
  if (state == GVState::IDLE) {
    if (!geminiLive.isReady()) {
      Serial.println("[GranVoice] tap ignored, Gemini not ready yet");
      lv_label_set_text(talkLabel, "Зачекайте...");
      return;
    }
    Serial.println("[GranVoice] -> LISTENING");
    lv_label_set_text(replyLabel, "");
    listenStartMs = millis();
    state = GVState::LISTENING;
    GranVoice_Audio_StartCapture();
    SetVisual(GVState::LISTENING);
  } else {
    CancelToIdle("tapped while active");
  }
}

// Runs on the LVGL task (lv_timer), so it's safe to touch LVGL objects here.
// GeminiLive's callbacks (different task) only ever set the plain flags above.
static void GranVoice_Tick(lv_timer_t*) {
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 2000) {
    lastWifiCheck = millis();
    UpdateWifiStatus();
    UpdateBatteryStatus();
  }

  // Physical BOOT button, polled with a simple debounce. Only acts as a talk
  // trigger when the user has selected that mode in settings.
  if (GetUseBootButton()) {
    static bool wasPressed = false;
    static unsigned long lastEdge = 0;
    bool pressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);
    if (pressed != wasPressed && millis() - lastEdge > 50) {
      lastEdge = millis();
      wasPressed = pressed;
      if (pressed) {
        Serial.println("[GranVoice] BOOT button pressed");
        OnButtonClicked(nullptr);
      }
    }
  }

  if (replyDirty) {
    portENTER_CRITICAL(&replyMux);
    replyDirty = false;
    static char snapshot[sizeof(pendingReply)];
    strncpy(snapshot, pendingReply, sizeof(snapshot));
    snapshot[sizeof(snapshot) - 1] = '\0';
    portEXIT_CRITICAL(&replyMux);
    lv_label_set_text(replyLabel, snapshot);
    // Keep the newest words visible as the reply streams in; the user can still
    // swipe back up through the container to re-read earlier text.
    lv_obj_scroll_to_y(replyBox, LV_COORD_MAX, LV_ANIM_OFF);
  }

  if (interruptedFlag) {
    interruptedFlag = false;
    CancelToIdle("interrupted");
    return;
  }

  if (audioArrivedFlag) {
    audioArrivedFlag = false;
    Serial.println("[GranVoice] -> SPEAKING (audio arrived)");
    SetVisual(GVState::SPEAKING);
  }

  if (state == GVState::LISTENING && millis() - listenStartMs > 15000) {
    CancelToIdle("15s listen timeout, no reply");
    return;
  }

  if (state == GVState::SPEAKING && turnCompleteFlag && GranVoice_Audio_IsPlaybackIdle()) {
    Serial.println("[GranVoice] -> IDLE (turn complete, playback drained)");
    turnCompleteFlag = false;
    state = GVState::IDLE;
    SetVisual(GVState::IDLE);
  }
}

static void BuildMainScreen() {
  mainScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(mainScreen, lv_color_black(), 0);
  lv_obj_clear_flag(mainScreen, LV_OBJ_FLAG_SCROLLABLE);

  wifiLabel = lv_label_create(mainScreen);
  lv_obj_set_style_text_font(wifiLabel, &lv_font_ua_22, 0);
  lv_obj_align(wifiLabel, LV_ALIGN_TOP_MID, 0, 30);
  lv_label_set_text(wifiLabel, "...");

  // Left edge at vertical centre - the widest part of the round panel, and
  // clear of the talk button which sits in the middle.
  batteryLabel = lv_label_create(mainScreen);
  lv_obj_set_style_text_font(batteryLabel, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(batteryLabel, lv_color_white(), 0);
  lv_obj_align(batteryLabel, LV_ALIGN_LEFT_MID, 6, -6);
  lv_label_set_text(batteryLabel, "");

  talkBtn = lv_btn_create(mainScreen);
  lv_obj_set_size(talkBtn, 200, 200);
  lv_obj_align(talkBtn, LV_ALIGN_CENTER, 0, -10);
  lv_obj_set_style_radius(talkBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_add_event_cb(talkBtn, OnButtonClicked, LV_EVENT_CLICKED, NULL);

  talkLabel = lv_label_create(talkBtn);
  lv_obj_set_style_text_align(talkLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(talkLabel, &lv_font_ua_22, 0);
  lv_obj_center(talkLabel);

  // Reply transcript in a scrollable container, so a long answer can be swiped
  // through instead of overflowing off the round panel.
  replyBox = lv_obj_create(mainScreen);
  lv_obj_set_style_bg_opa(replyBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(replyBox, 0, 0);
  lv_obj_set_style_pad_all(replyBox, 0, 0);
  lv_obj_set_scroll_dir(replyBox, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(replyBox, LV_SCROLLBAR_MODE_AUTO);

  replyLabel = lv_label_create(replyBox);
  lv_obj_set_style_text_font(replyLabel, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(replyLabel, lv_color_white(), 0);
  lv_obj_set_style_text_align(replyLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(replyLabel, LV_LABEL_LONG_WRAP);
  lv_obj_align(replyLabel, LV_ALIGN_TOP_MID, 0, 0);
  lv_label_set_text(replyLabel, "");

  lv_obj_t* gear = lv_btn_create(mainScreen);
  lv_obj_set_size(gear, 44, 44);
  lv_obj_align(gear, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(gear, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
  lv_obj_add_event_cb(gear, [](lv_event_t*) { GranVoice_Audio_PlayTap(); ShowSettings(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* gearLbl = lv_label_create(gear);
  lv_label_set_text(gearLbl, LV_SYMBOL_SETTINGS);
  lv_obj_center(gearLbl);

  ApplyMainLayout();
  SetVisual(GVState::IDLE);
}

// Two layouts sharing the same widgets: with the physical BOOT button doing the
// talking, the on-screen button shrinks to a status dot and the reply text takes
// over the freed space.
static void ApplyMainLayout() {
  if (!talkBtn || !replyBox) return;

  if (GetUseBootButton()) {
    lv_obj_set_size(talkBtn, 92, 92);
    lv_obj_align(talkBtn, LV_ALIGN_TOP_MID, 0, 66);

    lv_obj_set_size(replyBox, 250, 150);
    lv_obj_align(replyBox, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_set_width(replyLabel, 236);
  } else {
    lv_obj_set_size(talkBtn, 180, 180);
    lv_obj_align(talkBtn, LV_ALIGN_CENTER, 0, -28);

    lv_obj_set_size(replyBox, 250, 74);
    lv_obj_align(replyBox, LV_ALIGN_BOTTOM_MID, 0, -52);
    lv_obj_set_width(replyLabel, 236);
  }
}

static void ShowMain() {
  lv_scr_load(mainScreen);
  UpdateWifiStatus();
}

// ------------------------------------------------------------ settings screen

static void OnVolumeChanged(lv_event_t* e) {
  lv_obj_t* slider = lv_event_get_target(e);
  int v = lv_slider_get_value(slider);
  GranVoice_Audio_SetVolume(v);
  lv_obj_t* lbl = (lv_obj_t*)lv_event_get_user_data(e);
  static char buf[32];
  snprintf(buf, sizeof(buf), "Гучність: %d%%", v);
  lv_label_set_text(lbl, buf);
}

static void BuildSettingsScreen() {
  settingsScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(settingsScreen, lv_color_black(), 0);
  lv_obj_clear_flag(settingsScreen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(settingsScreen);
  lv_obj_set_style_text_font(title, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_label_set_text(title, "Налаштування");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

  // Scrolling column: there are more settings than fit on a 360px round panel,
  // and this keeps growing room without re-tuning every y offset by hand.
  lv_obj_t* list = lv_obj_create(settingsScreen);
  lv_obj_set_size(list, 280, 224);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 4, 0);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, 10, 0);

  // --- volume
  lv_obj_t* volLabel = lv_label_create(list);
  lv_obj_set_style_text_font(volLabel, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(volLabel, lv_color_white(), 0);
  static char volBuf[32];
  snprintf(volBuf, sizeof(volBuf), "Гучність: %d%%", GranVoice_Audio_GetVolume());
  lv_label_set_text(volLabel, volBuf);

  lv_obj_t* slider = lv_slider_create(list);
  lv_obj_set_width(slider, 240);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, GranVoice_Audio_GetVolume(), LV_ANIM_OFF);
  lv_obj_add_event_cb(slider, OnVolumeChanged, LV_EVENT_VALUE_CHANGED, volLabel);

  // --- backlight
  lv_obj_t* blLabel = lv_label_create(list);
  lv_obj_set_style_text_font(blLabel, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(blLabel, lv_color_white(), 0);
  static char blBuf[32];
  snprintf(blBuf, sizeof(blBuf), "Яскравість: %d%%", LCD_Backlight);
  lv_label_set_text(blLabel, blBuf);

  lv_obj_t* blSlider = lv_slider_create(list);
  lv_obj_set_width(blSlider, 240);
  lv_slider_set_range(blSlider, 10, 100); // never fully dark - it'd look broken
  lv_slider_set_value(blSlider, LCD_Backlight, LV_ANIM_OFF);
  lv_obj_add_event_cb(blSlider, [](lv_event_t* e) {
    lv_obj_t* sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    Set_Backlight((uint8_t)v);
    Preferences p;
    p.begin("granvoice", false);
    p.putInt("backlight", v);
    p.end();
    static char buf[32];
    snprintf(buf, sizeof(buf), "Яскравість: %d%%", v);
    lv_label_set_text((lv_obj_t*)lv_event_get_user_data(e), buf);
  }, LV_EVENT_VALUE_CHANGED, blLabel);

  // --- spoken replies on/off (text is always shown)
  lv_obj_t* speechCb = lv_checkbox_create(list);
  lv_checkbox_set_text(speechCb, "Озвучувати відповідь");
  lv_obj_set_style_text_font(speechCb, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(speechCb, lv_color_white(), 0);
  if (GranVoice_Audio_GetSpeechEnabled()) lv_obj_add_state(speechCb, LV_STATE_CHECKED);
  lv_obj_add_event_cb(speechCb, [](lv_event_t* e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    GranVoice_Audio_SetSpeechEnabled(on);
    GranVoice_Audio_PlayTap();
    Serial.printf("[GranVoice] speech replies %s\n", on ? "ON" : "OFF");
  }, LV_EVENT_VALUE_CHANGED, NULL);

  // --- talk trigger: touchscreen or physical BOOT button
  lv_obj_t* bootCb = lv_checkbox_create(list);
  lv_checkbox_set_text(bootCb, "Кнопка BOOT для розмови");
  lv_obj_set_style_text_font(bootCb, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(bootCb, lv_color_white(), 0);
  if (GetUseBootButton()) lv_obj_add_state(bootCb, LV_STATE_CHECKED);
  lv_obj_add_event_cb(bootCb, [](lv_event_t* e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    SetUseBootButton(on);
    SetVisual(GVState::IDLE);
    GranVoice_Audio_PlayTap();
    Serial.printf("[GranVoice] talk trigger: %s\n", on ? "BOOT button" : "touchscreen");
  }, LV_EVENT_VALUE_CHANGED, NULL);

  // --- wifi
  lv_obj_t* wifiBtn = lv_btn_create(list);
  lv_obj_set_size(wifiBtn, 240, 48);
  lv_obj_add_event_cb(wifiBtn, [](lv_event_t*) { GranVoice_Audio_PlayTap(); ShowWifiList(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* wifiBtnLbl = lv_label_create(wifiBtn);
  lv_obj_set_style_text_font(wifiBtnLbl, &lv_font_ua_22, 0);
  lv_label_set_text(wifiBtnLbl, LV_SYMBOL_WIFI " Мережа WiFi");
  lv_obj_center(wifiBtnLbl);

  lv_obj_t* backBtn = lv_btn_create(settingsScreen);
  lv_obj_set_size(backBtn, 150, 44);
  lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_set_style_bg_color(backBtn, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
  lv_obj_add_event_cb(backBtn, [](lv_event_t*) { GranVoice_Audio_PlayTap(); ShowMain(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* backLbl = lv_label_create(backBtn);
  lv_obj_set_style_text_font(backLbl, &lv_font_ua_22, 0);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Назад");
  lv_obj_center(backLbl);
}

static void ShowSettings() {
  lv_scr_load(settingsScreen);
}

// ----------------------------------------------------------- wifi list screen

static void OnNetworkPicked(lv_event_t* e) {
  GranVoice_Audio_PlayTap();
  lv_obj_t* btn = lv_event_get_target(e);
  const char* ssid = lv_list_get_btn_text(lv_obj_get_parent(btn), btn);
  strncpy(pendingSSID, ssid, sizeof(pendingSSID) - 1);
  pendingSSID[sizeof(pendingSSID) - 1] = '\0';

  static char titleBuf[96];
  snprintf(titleBuf, sizeof(titleBuf), "Пароль для %s", pendingSSID);
  lv_label_set_text(passwordTitle, titleBuf);
  lv_textarea_set_text(passwordTA, "");
  lv_scr_load(passwordScreen);
}

static void ShowWifiList() {
  // Rebuilt on each entry so the scan results are always current.
  if (wifiListScreen) {
    lv_obj_del(wifiListScreen);
    wifiListScreen = nullptr;
  }
  wifiListScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(wifiListScreen, lv_color_black(), 0);
  lv_obj_clear_flag(wifiListScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_scr_load(wifiListScreen);

  lv_obj_t* status = lv_label_create(wifiListScreen);
  lv_obj_set_style_text_font(status, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(status, lv_color_white(), 0);
  lv_label_set_text(status, "Пошук мереж...");
  lv_obj_center(status);
  lv_refr_now(NULL); // paint the "scanning" message before the blocking scan

  int n = WiFi.scanNetworks();
  lv_obj_del(status);

  lv_obj_t* list = lv_list_create(wifiListScreen);
  lv_obj_set_size(list, 300, 250);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_style_bg_color(list, lv_palette_darken(LV_PALETTE_GREY, 4), 0);

  for (int i = 0; i < n && i < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    lv_obj_t* btn = lv_list_add_btn(list, LV_SYMBOL_WIFI, ssid.c_str());
    lv_obj_set_style_text_font(btn, &lv_font_ua_22, 0);
    lv_obj_add_event_cb(btn, OnNetworkPicked, LV_EVENT_CLICKED, NULL);
  }
  if (n <= 0) {
    lv_obj_t* none = lv_list_add_text(list, "Мереж не знайдено");
    lv_obj_set_style_text_font(none, &lv_font_ua_22, 0);
  }

  lv_obj_t* backBtn = lv_btn_create(wifiListScreen);
  lv_obj_set_size(backBtn, 150, 44);
  lv_obj_align(backBtn, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_obj_set_style_bg_color(backBtn, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
  lv_obj_add_event_cb(backBtn, [](lv_event_t*) { GranVoice_Audio_PlayTap(); ShowSettings(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* backLbl = lv_label_create(backBtn);
  lv_obj_set_style_text_font(backLbl, &lv_font_ua_22, 0);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT " Назад");
  lv_obj_center(backLbl);
}

// ------------------------------------------------------- password entry screen

static void TryConnectWithPassword() {
  const char* pass = lv_textarea_get_text(passwordTA);
  lv_label_set_text(passwordTitle, "Підключення...");
  lv_refr_now(NULL);

  bool ok = wifiManager.connectAndSave(String(pendingSSID), String(pass));

  if (ok) {
    // Gemini's socket belongs to the old connection - redial on the new one.
    geminiLive.reconnect();
    ShowMain();
  } else {
    lv_label_set_text(passwordTitle, "Не вдалося. Спробуйте ще раз");
  }
}

static void OnKeyboardEvent(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CANCEL) {
    ShowWifiList();
  } else if (code == LV_EVENT_READY) {
    TryConnectWithPassword();
  }
}

static void BuildPasswordScreen() {
  passwordScreen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(passwordScreen, lv_color_black(), 0);
  lv_obj_clear_flag(passwordScreen, LV_OBJ_FLAG_SCROLLABLE);

  passwordTitle = lv_label_create(passwordScreen);
  lv_obj_set_style_text_font(passwordTitle, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(passwordTitle, lv_color_white(), 0);
  lv_label_set_long_mode(passwordTitle, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(passwordTitle, 300);
  lv_obj_set_style_text_align(passwordTitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(passwordTitle, LV_ALIGN_TOP_MID, 0, 20);
  lv_label_set_text(passwordTitle, "Пароль");

  passwordTA = lv_textarea_create(passwordScreen);
  lv_textarea_set_one_line(passwordTA, true);
  lv_textarea_set_password_mode(passwordTA, false); // visible: easier to fix typos
  lv_obj_set_width(passwordTA, 250);
  lv_obj_align(passwordTA, LV_ALIGN_TOP_MID, 0, 62);

  // Explicit Back / Connect buttons: the keyboard's own checkmark and X sit in
  // its top corners, which the round bezel clips off on this display.
  lv_obj_t* cancelBtn = lv_btn_create(passwordScreen);
  lv_obj_set_size(cancelBtn, 96, 40);
  lv_obj_align(cancelBtn, LV_ALIGN_TOP_LEFT, 58, 108);
  lv_obj_set_style_bg_color(cancelBtn, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
  lv_obj_add_event_cb(cancelBtn, [](lv_event_t*) { GranVoice_Audio_PlayTap(); ShowWifiList(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
  lv_obj_set_style_text_font(cancelLbl, &lv_font_ua_22, 0);
  lv_label_set_text(cancelLbl, LV_SYMBOL_LEFT " Назад");
  lv_obj_center(cancelLbl);

  lv_obj_t* okBtn = lv_btn_create(passwordScreen);
  lv_obj_set_size(okBtn, 116, 40);
  lv_obj_align(okBtn, LV_ALIGN_TOP_RIGHT, -50, 108);
  lv_obj_set_style_bg_color(okBtn, lv_palette_main(LV_PALETTE_GREEN), 0);
  lv_obj_add_event_cb(okBtn, [](lv_event_t*) { GranVoice_Audio_PlayTap(); TryConnectWithPassword(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* okLbl = lv_label_create(okBtn);
  lv_obj_set_style_text_font(okLbl, &lv_font_ua_22, 0);
  lv_label_set_text(okLbl, LV_SYMBOL_OK " Готово");
  lv_obj_center(okLbl);

  lv_obj_t* kb = lv_keyboard_create(passwordScreen);
  lv_obj_set_size(kb, 360, 190);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(kb, passwordTA);
  lv_obj_add_event_cb(kb, OnKeyboardEvent, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(kb, OnKeyboardEvent, LV_EVENT_CANCEL, NULL);
}

// ------------------------------------------------------------------- lifecycle

void GranVoice_UI_Init(void) {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  BuildMainScreen();
  BuildSettingsScreen();
  BuildPasswordScreen();
  ShowMain();

  geminiLive.onAudio([](const uint8_t* pcm, size_t len) {
    if (state == GVState::LISTENING) {
      GranVoice_Audio_StopCapture();
      state = GVState::SPEAKING;
      audioArrivedFlag = true; // GranVoice_Tick (LVGL task) does the actual SetVisual()
    }
    GranVoice_Audio_QueuePlayback(pcm, len);
  });
  geminiLive.onTurnComplete([]() { turnCompleteFlag = true; });
  geminiLive.onInterrupted([]() { interruptedFlag = true; });

  // Gemini streams the reply transcript in fragments; append them so the label
  // shows the whole sentence rather than the last few words.
  geminiLive.onSaying([](const char* text) {
    portENTER_CRITICAL(&replyMux);
    size_t used = strlen(pendingReply);
    strncat(pendingReply, text, sizeof(pendingReply) - used - 1);
    replyDirty = true;
    portEXIT_CRITICAL(&replyMux);
  });
  geminiLive.onHeard([](const char*) {
    portENTER_CRITICAL(&replyMux);
    pendingReply[0] = '\0'; // new question - clear the previous answer
    replyDirty = true;
    portEXIT_CRITICAL(&replyMux);
  });

  lv_timer_create(GranVoice_Tick, 100, NULL);
}

void GranVoice_UI_ShowWifiSetup(void) {
  ShowWifiList();
}

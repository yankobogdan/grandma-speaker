#include "GranVoice_UI.h"
#include "GranVoice_Audio.h"
#include "GeminiLive.h"
#include "LVGL_Driver.h"
#include "WiFi_Manager.h"
#include "BAT_Driver.h"
#include <WiFi.h>
#include <Preferences.h>

// ESP32-S3 BOOT button. Usable as a normal input once booted; pressed reads LOW.
// Held for LOCK_HOLD_MS it toggles the touchscreen lock, so the screen can be
// wiped or carried without triggering a conversation.
#define BOOT_BUTTON_PIN 0
#define LOCK_HOLD_MS 2000

// External push-to-talk button, wired to GND (internal pull-up, pressed = LOW).
// NOTE: GPIO44 is UART0 RX. Free here because the console runs over USB CDC,
// but don't attach a serial adapter to the UART header while using it.
#define TALK_BUTTON_PIN 44

// Read by the LVGL touch driver: while set, touch input is swallowed before
// LVGL ever sees it. Gating only the talk button left the rest of the UI live.
bool granvoice_touch_locked = false;

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
static volatile bool sessionLostFlag = false;
static unsigned long listenStartMs = 0;

// Reply/heard text is produced on the WS task but may only be rendered on the
// LVGL task, so it lands in these buffers and GranVoice_Tick paints them.
static char pendingReply[512] = {0};
static char pendingHeard[256] = {0};   // transcript of what the user said
static volatile bool replyDirty = false;
static volatile bool heardDirty = false;
static portMUX_TYPE replyMux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t* mainScreen = nullptr;
static lv_obj_t* talkBtn = nullptr;
static lv_obj_t* talkLabel = nullptr;
static lv_obj_t* heardLabel = nullptr; // live bubble for the current question
static lv_obj_t* chatList = nullptr;   // scrollable conversation
static lv_obj_t* replyBubble = nullptr; // live bubble for the current answer

// Two pre-built bubbles, reused every turn. Creating a bubble per turn meant
// LVGL object churn plus a flex relayout on every transcript fragment, and this
// panel requires full_refresh, so each of those repainted all 360x360 pixels.
// That combination made the UI unusable. Reusing fixed objects keeps the cost
// of an update to "set one label's text".
static lv_obj_t* MakeBubble(bool fromUser) {
  lv_obj_t* wrap = lv_obj_create(chatList);
  lv_obj_set_width(wrap, 280);
  lv_obj_set_height(wrap, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(wrap,
      fromUser ? lv_palette_darken(LV_PALETTE_BLUE_GREY, 3)
               : lv_palette_darken(LV_PALETTE_TEAL, 4), 0);
  lv_obj_set_style_border_width(wrap, 0, 0);
  lv_obj_set_style_radius(wrap, 10, 0);
  lv_obj_set_style_pad_all(wrap, 8, 0);
  lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl = lv_label_create(wrap);
  lv_obj_set_style_text_font(lbl, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(lbl,
      fromUser ? lv_palette_lighten(LV_PALETTE_BLUE_GREY, 4) : lv_color_white(), 0);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, 264);
  lv_label_set_text(lbl, "");
  lv_obj_add_flag(wrap, LV_OBJ_FLAG_HIDDEN); // shown once it has text
  return lbl;
}

static void ChatClear() {
  if (heardLabel) {
    lv_label_set_text(heardLabel, "");
    lv_obj_add_flag(lv_obj_get_parent(heardLabel), LV_OBJ_FLAG_HIDDEN);
  }
  if (replyBubble) {
    lv_label_set_text(replyBubble, "");
    lv_obj_add_flag(lv_obj_get_parent(replyBubble), LV_OBJ_FLAG_HIDDEN);
  }
}

// Sets a bubble's text and reveals it on first use.
static void BubbleSet(lv_obj_t* lbl, const char* text) {
  if (!lbl) return;
  lv_label_set_text(lbl, text);
  lv_obj_t* wrap = lv_obj_get_parent(lbl);
  if (text && text[0]) lv_obj_clear_flag(wrap, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t* wifiLabel = nullptr;
static lv_obj_t* batteryLabel = nullptr;
static lv_obj_t* usageLabel = nullptr;

// ---- streamed-voice usage counter -------------------------------------------
// Counts seconds of microphone audio actually sent to Gemini, persisted in NVS.
// Google's free-tier quotas roll over at midnight Pacific, so the counter is
// keyed on the Pacific date and resets when that date changes - matching the
// reset the user is actually budgeting against.
static uint32_t usageSecondsToday = 0;
static char usageDay[12] = {0};      // "YYYY-MM-DD" in Pacific time
static uint32_t usagePendingMs = 0;  // accumulated but not yet persisted

// Pacific date, or "" until NTP has supplied a plausible time.
static void PacificDateString(char* out, size_t outLen) {
  out[0] = '\0';
  time_t now = time(nullptr);
  if (now < 1700000000) return; // clock not set yet
  struct tm tmv;
  if (!localtime_r(&now, &tmv)) return;
  strftime(out, outLen, "%Y-%m-%d", &tmv);
}

static void UsageLoad() {
  Preferences p;
  p.begin("granvoice", true);
  usageSecondsToday = p.getUInt("use_secs", 0);
  String d = p.getString("use_day", "");
  p.end();
  strncpy(usageDay, d.c_str(), sizeof(usageDay) - 1);
  usageDay[sizeof(usageDay) - 1] = '\0';
}

static void UsageSave() {
  Preferences p;
  p.begin("granvoice", false);
  p.putUInt("use_secs", usageSecondsToday);
  p.putString("use_day", usageDay);
  p.end();
}

// Rolls the counter over when the Pacific date changes.
static void UsageRolloverCheck() {
  char today[12];
  PacificDateString(today, sizeof(today));
  if (today[0] == '\0') return;             // no valid clock yet
  if (usageDay[0] == '\0') {                 // first run with a valid clock
    strncpy(usageDay, today, sizeof(usageDay) - 1);
    UsageSave();
    return;
  }
  if (strcmp(today, usageDay) != 0) {
    Serial.printf("[Usage] new Pacific day %s -> %s, resetting %us\n",
                  usageDay, today, usageSecondsToday);
    usageSecondsToday = 0;
    strncpy(usageDay, today, sizeof(usageDay) - 1);
    UsageSave();
  }
}

// Called with however long the mic was streaming for this turn.
static void UsageAddMs(uint32_t ms) {
  usagePendingMs += ms;
  if (usagePendingMs >= 1000) {
    usageSecondsToday += usagePendingMs / 1000;
    usagePendingMs %= 1000;
    UsageSave(); // once per turn, not per second - easy on the flash
  }
}

// lv_label_set_text always invalidates the object, even when the string is
// identical, and with disp_drv.full_refresh a single dirty label repaints the
// entire 360x360 screen: 259KB pushed through the SPI driver's internal-RAM
// bounce buffers, 127 malloc/free pairs. Doing that every 5 seconds for a
// battery percentage and an SSID that almost never change fragmented internal
// DRAM until mbedtls could no longer allocate its buffers, at which point every
// Gemini reconnect failed with "SSL - Memory allocation failed (-32512)" and the
// device was dead until a reboot. So: only touch the widget when it would
// actually look different.
static void SetLabelIfChanged(lv_obj_t* label, const char* text) {
  if (!label) return;
  const char* cur = lv_label_get_text(label);
  if (cur && strcmp(cur, text) == 0) return;
  lv_label_set_text(label, text);
}

static void SetTextColorIfChanged(lv_obj_t* obj, lv_color_t col) {
  if (!obj) return;
  if (lv_obj_get_style_text_color(obj, LV_PART_MAIN).full == col.full) return;
  lv_obj_set_style_text_color(obj, col, 0);
}

static void UpdateUsageLabel() {
  if (!usageLabel) return;
  static char buf[32];
  uint32_t mins = usageSecondsToday / 60;
  uint32_t secs = usageSecondsToday % 60;
  if (mins > 0) snprintf(buf, sizeof(buf), LV_SYMBOL_UPLOAD " %um", (unsigned)mins);
  else          snprintf(buf, sizeof(buf), LV_SYMBOL_UPLOAD " %us", (unsigned)secs);
  SetLabelIfChanged(usageLabel, buf);
}

// Compact layout: shrinks the on-screen talk button to a status dot and gives
// the freed space to the reply text. Useful when the GPIO44 button is doing the
// talking, so the big touch target isn't needed.
static int useBootButton = -1; // -1 = not yet loaded from NVS (NVS key kept for compatibility)

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
  SetLabelIfChanged(batteryLabel, buf);

  lv_color_t col = (pct <= 15) ? lv_palette_main(LV_PALETTE_RED)
                 : (pct <= 35) ? lv_palette_main(LV_PALETTE_ORANGE)
                               : lv_color_white();
  SetTextColorIfChanged(batteryLabel, col);
}

static void UpdateWifiStatus() {
  if (!wifiLabel) return;
  // Quota refusal outranks the WiFi indicator: the network is fine, but nothing
  // will work, and "connected" alone would be misleading.
  if (geminiLive.isQuotaExhausted()) {
    SetLabelIfChanged(wifiLabel, LV_SYMBOL_WARNING " Ліміт Gemini вичерпано");
    SetTextColorIfChanged(wifiLabel, lv_palette_main(LV_PALETTE_RED));
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    static char buf[80];
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", WiFi.SSID().c_str());
    SetLabelIfChanged(wifiLabel, buf);
    SetTextColorIfChanged(wifiLabel, lv_palette_main(LV_PALETTE_GREEN));
  } else {
    SetLabelIfChanged(wifiLabel, LV_SYMBOL_WARNING " Немає WiFi");
    SetTextColorIfChanged(wifiLabel, lv_palette_main(LV_PALETTE_ORANGE));
  }
}

// Brief centred banner, used for lock state changes.
static lv_obj_t* toastObj = nullptr;

static void ToastExpire(lv_timer_t*) {
  // Only the timer clears the toast, and it nulls the pointer as it goes.
  // Deleting the timer here as well would double-free it: a repeat count of 1
  // already makes LVGL dispose of it, and the stale pointer left behind by the
  // previous version crashed the device on the next toast.
  if (toastObj) {
    lv_obj_del(toastObj);
    toastObj = nullptr;
  }
}

static void ShowToast(const char* text, lv_color_t colour) {
  if (toastObj) { lv_obj_del(toastObj); toastObj = nullptr; }
  toastObj = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_color(toastObj, colour, 0);
  lv_obj_set_style_bg_opa(toastObj, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(toastObj, lv_color_white(), 0);
  lv_obj_set_style_text_font(toastObj, &lv_font_ua_22, 0);
  lv_obj_set_style_text_align(toastObj, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_all(toastObj, 14, 0);
  lv_obj_set_style_radius(toastObj, 10, 0);
  lv_label_set_text(toastObj, text);
  lv_obj_center(toastObj);

  lv_timer_t* t = lv_timer_create(ToastExpire, 1600, nullptr);
  lv_timer_set_repeat_count(t, 1); // LVGL frees the timer itself afterwards
}

static void SetScreenLocked(bool locked) {
  granvoice_touch_locked = locked;
  if (locked) {
    ShowToast(LV_SYMBOL_CLOSE "  Екран заблоковано", lv_palette_darken(LV_PALETTE_BLUE_GREY, 2));
  } else {
    ShowToast(LV_SYMBOL_OK "  Екран розблоковано", lv_palette_main(LV_PALETTE_GREEN));
  }
  GranVoice_Audio_PlayTap();
  Serial.printf("[GranVoice] touchscreen %s\n", locked ? "LOCKED" : "UNLOCKED");
}

static void CancelToIdle(const char* reason) {
  Serial.printf("[GranVoice] -> IDLE (%s)\n", reason);
  GranVoice_Audio_ReplyEnd();
  if (state == GVState::LISTENING) UsageAddMs(millis() - listenStartMs);
  GranVoice_Audio_StopCapture();
  geminiLive.sendAudioStreamEnd();
  GranVoice_Audio_FlushPlayback();
  turnCompleteFlag = false;
  state = GVState::IDLE;
  SetVisual(GVState::IDLE);
}

static void OnButtonClicked(lv_event_t* e) {
  // e == nullptr means a physical button, which stays live while the
  // touchscreen is locked - locking is about stray touches, not disabling the device.
  if (granvoice_touch_locked && e != nullptr) {
    Serial.println("[GranVoice] touch ignored - screen locked");
    ShowToast(LV_SYMBOL_CLOSE "  Екран заблоковано\nутримуйте BOOT", lv_palette_darken(LV_PALETTE_BLUE_GREY, 2));
    return;
  }
  Serial.println("[GranVoice] button tapped");
  GranVoice_Audio_PlayTap();
  if (state == GVState::IDLE) {
    if (!geminiLive.isReady()) {
      // Say *why* it isn't ready: "please wait" alone gave no clue whether the
      // problem was the network, the quota, or just a slow connect.
      Serial.println("[GranVoice] tap ignored, Gemini not ready yet");
      if (WiFi.status() != WL_CONNECTED) {
        ShowToast(LV_SYMBOL_WARNING "  Немає WiFi\nПідключення...", lv_palette_main(LV_PALETTE_ORANGE));
      } else if (geminiLive.isQuotaExhausted()) {
        ShowToast(LV_SYMBOL_WARNING "  Ліміт Gemini\nвичерпано", lv_palette_main(LV_PALETTE_RED));
      } else {
        ShowToast(LV_SYMBOL_REFRESH "  Підключення до Gemini...", lv_palette_darken(LV_PALETTE_BLUE_GREY, 2));
      }
      return;
    }
    Serial.println("[GranVoice] -> LISTENING");
    ChatClear();
    if (replyBubble) lv_obj_set_style_text_color(replyBubble, lv_color_white(), 0);
    portENTER_CRITICAL(&replyMux);
    pendingHeard[0] = '\0';
    pendingReply[0] = '\0';
    portEXIT_CRITICAL(&replyMux);
    listenStartMs = millis();
    state = GVState::LISTENING;
    geminiLive.beginTurn(); // re-arm audio sending after the previous streamEnd
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
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    UpdateWifiStatus();
    UpdateBatteryStatus();
    UsageRolloverCheck();
    UpdateUsageLabel();
  }

  // BOOT button: a long hold toggles the touchscreen lock. A short press is
  // ignored so it can't be mistaken for a talk trigger.
  {
    static bool bootWasDown = false;
    static unsigned long bootDownAt = 0;
    static bool lockFiredThisHold = false;
    bool bootDown = (digitalRead(BOOT_BUTTON_PIN) == LOW);

    if (bootDown && !bootWasDown) {
      bootDownAt = millis();
      lockFiredThisHold = false;
    } else if (bootDown && !lockFiredThisHold && millis() - bootDownAt >= LOCK_HOLD_MS) {
      lockFiredThisHold = true; // fire once per hold, not repeatedly
      SetScreenLocked(!granvoice_touch_locked);
    }
    bootWasDown = bootDown;
  }

  // GPIO44 push-to-talk, mirroring a tap on the on-screen button. Works while
  // the screen is locked, which is the point of having it.
  {
    static bool talkWasDown = false;
    static unsigned long talkEdgeAt = 0;
    bool talkDown = (digitalRead(TALK_BUTTON_PIN) == LOW);
    if (talkDown != talkWasDown && millis() - talkEdgeAt > 50) { // debounce
      talkEdgeAt = millis();
      talkWasDown = talkDown;
      if (talkDown) {
        Serial.println("[GranVoice] GPIO44 button pressed");
        OnButtonClicked(nullptr);
      }
    }
  }

  // Transcript fragments arrive many times a second. Repainting on each one
  // costs a redraw + SPI transfer that competes with audio playback, so the
  // text is coalesced and applied at a steady, modest rate instead.
  static unsigned long lastTextPaint = 0;
  // full_refresh is mandatory on this panel, so every text change repaints all
  // 360x360 pixels. Coalescing hard keeps the SPI bus free for audio; twice a
  // second still reads as live.
  bool mayPaintText = (millis() - lastTextPaint) >= 500;

  if (heardDirty && mayPaintText) {
    lastTextPaint = millis();
    portENTER_CRITICAL(&replyMux);
    heardDirty = false;
    static char hsnap[sizeof(pendingHeard)];
    strncpy(hsnap, pendingHeard, sizeof(hsnap));
    hsnap[sizeof(hsnap) - 1] = '\0';
    portEXIT_CRITICAL(&replyMux);
    if (hsnap[0]) {
      // One bubble per question, updated as the transcript streams in.
      BubbleSet(heardLabel, hsnap);
    }
  }

  if (replyDirty && mayPaintText) {
    lastTextPaint = millis();
    portENTER_CRITICAL(&replyMux);
    replyDirty = false;
    static char snapshot[sizeof(pendingReply)];
    strncpy(snapshot, pendingReply, sizeof(snapshot));
    snapshot[sizeof(snapshot) - 1] = '\0';
    portEXIT_CRITICAL(&replyMux);
    if (snapshot[0]) {
      BubbleSet(replyBubble, snapshot);
    }
  }

  // Server dropped the session mid-turn. Say so plainly and point at the fix,
  // instead of dropping to idle in silence as if nothing had been asked.
  if (sessionLostFlag) {
    sessionLostFlag = false;
    if (state != GVState::IDLE) {
      GranVoice_Audio_ReplyEnd();
      GranVoice_Audio_StopCapture();
      GranVoice_Audio_FlushPlayback();
      turnCompleteFlag = false;
      state = GVState::IDLE;
      SetVisual(GVState::IDLE);
    }
    portENTER_CRITICAL(&replyMux);
    pendingReply[0] = '\0';
    replyDirty = false;
    portEXIT_CRITICAL(&replyMux);
    BubbleSet(replyBubble, "Зв'язок перервано. Натисніть і спитайте ще раз.");
    if (replyBubble) lv_obj_set_style_text_color(replyBubble, lv_palette_main(LV_PALETTE_ORANGE), 0);
    Serial.println("[GranVoice] session lost mid-turn - told the user to retry");
    return;
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
  lv_obj_align(wifiLabel, LV_ALIGN_TOP_MID, 0, 16);
  lv_label_set_text(wifiLabel, "...");

  // Left edge at vertical centre - the widest part of the round panel, and
  // clear of the talk button which sits in the middle.
  batteryLabel = lv_label_create(mainScreen);
  lv_obj_set_style_text_font(batteryLabel, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(batteryLabel, lv_color_white(), 0);
  lv_obj_align(batteryLabel, LV_ALIGN_TOP_MID, -62, 44);
  lv_label_set_text(batteryLabel, "");

  // Streamed-voice total for the current Pacific day, mirrored opposite the
  // battery so both status readouts sit clear of the talk button.
  usageLabel = lv_label_create(mainScreen);
  lv_obj_set_style_text_font(usageLabel, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(usageLabel, lv_palette_main(LV_PALETTE_BLUE_GREY), 0);
  lv_obj_align(usageLabel, LV_ALIGN_TOP_MID, 62, 44);
  lv_label_set_text(usageLabel, "");

  talkBtn = lv_btn_create(mainScreen);
  lv_obj_set_size(talkBtn, 200, 200);
  lv_obj_align(talkBtn, LV_ALIGN_CENTER, 0, -10);
  lv_obj_set_style_radius(talkBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_add_event_cb(talkBtn, OnButtonClicked, LV_EVENT_CLICKED, NULL);

  talkLabel = lv_label_create(talkBtn);
  lv_obj_set_style_text_align(talkLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(talkLabel, &lv_font_ua_22, 0);
  lv_obj_center(talkLabel);

  // Scrollable conversation: question and answer bubbles accumulate so earlier
  // turns can be read back, instead of each reply replacing the last.
  chatList = lv_obj_create(mainScreen);
  lv_obj_set_style_bg_opa(chatList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chatList, 0, 0);
  lv_obj_set_style_pad_all(chatList, 2, 0);
  lv_obj_set_style_pad_row(chatList, 6, 0);
  lv_obj_set_scroll_dir(chatList, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(chatList, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(chatList, LV_FLEX_FLOW_COLUMN);

  heardLabel  = MakeBubble(true);   // question
  replyBubble = MakeBubble(false);  // answer

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
  if (!talkBtn || !chatList) return;

  if (GetUseBootButton()) {
    lv_obj_set_size(talkBtn, 76, 76);
    lv_obj_align(talkBtn, LV_ALIGN_TOP_MID, 0, 74);

    lv_obj_set_size(chatList, 300, 152);
    lv_obj_align(chatList, LV_ALIGN_TOP_MID, 0, 158);
  } else {
    lv_obj_set_size(talkBtn, 148, 148);
    lv_obj_align(talkBtn, LV_ALIGN_CENTER, 0, -46);

    lv_obj_set_size(chatList, 300, 104);
    lv_obj_align(chatList, LV_ALIGN_BOTTOM_MID, 0, -44);
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
  lv_checkbox_set_text(bootCb, "Компактна кнопка розмови");
  lv_obj_set_style_text_font(bootCb, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(bootCb, lv_color_white(), 0);
  if (GetUseBootButton()) lv_obj_add_state(bootCb, LV_STATE_CHECKED);
  lv_obj_add_event_cb(bootCb, [](lv_event_t* e) {
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    SetUseBootButton(on);
    SetVisual(GVState::IDLE);
    GranVoice_Audio_PlayTap();
    Serial.printf("[GranVoice] compact talk button: %s\n", on ? "ON" : "OFF");
  }, LV_EVENT_VALUE_CHANGED, NULL);

  // --- voice. Gemini fixes the voice when the session is set up, so choosing a
  // new one necessarily starts a fresh session (and clears the conversation).
  lv_obj_t* voiceLabel = lv_label_create(list);
  lv_obj_set_style_text_font(voiceLabel, &lv_font_ua_22, 0);
  lv_obj_set_style_text_color(voiceLabel, lv_color_white(), 0);
  lv_label_set_text(voiceLabel, "Голос");

  lv_obj_t* voiceDd = lv_dropdown_create(list);
  lv_obj_set_width(voiceDd, 240);
  lv_obj_set_style_text_font(voiceDd, &lv_font_ua_22, 0);
  // A curated subset of Gemini's 30 prebuilt voices - the warm, gentle,
  // clearly-spoken ones suit an elderly listener better than the whole list.
  lv_dropdown_set_options(voiceDd,
      "Sulafat\nVindemiatrix\nAchird\nAchernar\nKore\nGacrux\nSchedar\nAoede");
  {
    const char* voices[] = {"Sulafat","Vindemiatrix","Achird","Achernar","Kore","Gacrux","Schedar","Aoede"};
    String cur = geminiLive.getVoice();
    for (int i = 0; i < 8; i++) if (cur == voices[i]) { lv_dropdown_set_selected(voiceDd, i); break; }
  }
  lv_obj_add_event_cb(voiceDd, [](lv_event_t* e) {
    static const char* voices[] = {"Sulafat","Vindemiatrix","Achird","Achernar","Kore","Gacrux","Schedar","Aoede"};
    uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
    if (sel >= 8) return;
    GranVoice_Audio_PlayTap();
    ChatClear();
    geminiLive.setVoice(voices[sel]); // restarts the session to take effect
    ShowToast(LV_SYMBOL_OK "  Голос змінено\nНову розмову почато", lv_palette_main(LV_PALETTE_GREEN));
  }, LV_EVENT_VALUE_CHANGED, NULL);

  // --- start a new conversation
  lv_obj_t* clearBtn = lv_btn_create(list);
  lv_obj_set_size(clearBtn, 240, 48);
  lv_obj_set_style_bg_color(clearBtn, lv_palette_darken(LV_PALETTE_DEEP_ORANGE, 2), 0);
  lv_obj_add_event_cb(clearBtn, [](lv_event_t*) {
    GranVoice_Audio_PlayTap();
    ChatClear();
    geminiLive.clearSession();
    ShowToast(LV_SYMBOL_REFRESH "  Нову розмову почато", lv_palette_main(LV_PALETTE_GREEN));
    ShowMain();
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* clearLbl = lv_label_create(clearBtn);
  lv_obj_set_style_text_font(clearLbl, &lv_font_ua_22, 0);
  lv_label_set_text(clearLbl, LV_SYMBOL_REFRESH " Нова розмова");
  lv_obj_center(clearLbl);

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

// Scanned SSIDs kept verbatim: the button label may have a "saved" tick
// appended, so it can't be read back as the network name.
static char scannedSSIDs[20][33];

static void OnNetworkPicked(lv_event_t* e) {
  GranVoice_Audio_PlayTap();
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= 20) return;
  const char* ssid = scannedSSIDs[idx];
  strncpy(pendingSSID, ssid, sizeof(pendingSSID) - 1);
  pendingSSID[sizeof(pendingSSID) - 1] = '\0';

  static char titleBuf[96];
  snprintf(titleBuf, sizeof(titleBuf), "Пароль для %s", pendingSSID);
  lv_label_set_text(passwordTitle, titleBuf);
  lv_textarea_set_text(passwordTA, "");
  lv_scr_load(passwordScreen);
}

static void ShowWifiList() {
  // Rebuilt on each entry so results are current; also reachable from the
  // Refresh button below, which is what "drag to refresh" was reaching for.
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
  lv_refr_now(NULL); // paint before the blocking scan

  // Drop any cached results first: a stale scan (common right after a failed
  // connection attempt) returns a negative code that used to be rendered as
  // "no networks found" with no way to retry.
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  int n = WiFi.scanNetworks();
  if (n < 0) {
    Serial.printf("[WiFi] scan failed (%d), retrying\n", n);
    WiFi.scanDelete();
    delay(400);
    n = WiFi.scanNetworks();
  }
  Serial.printf("[WiFi] scan returned %d\n", n);
  lv_obj_del(status);

  lv_obj_t* list = lv_list_create(wifiListScreen);
  lv_obj_set_size(list, 300, 210);
  lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_set_style_bg_color(list, lv_palette_darken(LV_PALETTE_GREY, 4), 0);

  int shown = 0;
  for (int i = 0; i < n && shown < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    // A tick marks networks already stored, so it's clear which ones the
    // device can join on its own.
    bool saved = wifiManager.isSaved(ssid);
    strncpy(scannedSSIDs[shown], ssid.c_str(), sizeof(scannedSSIDs[0]) - 1);
    scannedSSIDs[shown][sizeof(scannedSSIDs[0]) - 1] = '\0';
    String label = saved ? (ssid + "  " + String(LV_SYMBOL_OK)) : ssid;
    lv_obj_t* btn = lv_list_add_btn(list, LV_SYMBOL_WIFI, label.c_str());
    lv_obj_set_style_text_font(btn, &lv_font_ua_22, 0);
    lv_obj_add_event_cb(btn, OnNetworkPicked, LV_EVENT_CLICKED, (void*)(intptr_t)shown);
    shown++;
  }
  if (shown == 0) {
    lv_obj_t* none = lv_list_add_text(list,
      n < 0 ? "Помилка пошуку. Оновіть." : "Мереж не знайдено");
    lv_obj_set_style_text_font(none, &lv_font_ua_22, 0);
  }

  lv_obj_t* refreshBtn = lv_btn_create(wifiListScreen);
  lv_obj_set_size(refreshBtn, 140, 42);
  lv_obj_align(refreshBtn, LV_ALIGN_BOTTOM_LEFT, 34, -18);
  lv_obj_add_event_cb(refreshBtn, [](lv_event_t*) { GranVoice_Audio_PlayTap(); ShowWifiList(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* refreshLbl = lv_label_create(refreshBtn);
  lv_obj_set_style_text_font(refreshLbl, &lv_font_ua_22, 0);
  lv_label_set_text(refreshLbl, LV_SYMBOL_REFRESH " Оновити");
  lv_obj_center(refreshLbl);

  lv_obj_t* backBtn = lv_btn_create(wifiListScreen);
  lv_obj_set_size(backBtn, 120, 42);
  lv_obj_align(backBtn, LV_ALIGN_BOTTOM_RIGHT, -34, -18);
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
  pinMode(TALK_BUTTON_PIN, INPUT_PULLUP);
  UsageLoad();
  BuildMainScreen();
  BuildSettingsScreen();
  BuildPasswordScreen();
  ShowMain();

  geminiLive.onAudio([](const uint8_t* pcm, size_t len) {
    GranVoice_Audio_ReplyBegin(); // audio still arriving: gaps are not the end
    if (state == GVState::LISTENING) {
      UsageAddMs(millis() - listenStartMs); // mic stops here, so bank the time
      GranVoice_Audio_StopCapture();
      state = GVState::SPEAKING;
      audioArrivedFlag = true; // GranVoice_Tick (LVGL task) does the actual SetVisual()
    }
    GranVoice_Audio_QueuePlayback(pcm, len);
  });
  geminiLive.onTurnComplete([]() { GranVoice_Audio_ReplyEnd(); turnCompleteFlag = true; });
  geminiLive.onInterrupted([]() { interruptedFlag = true; });
  geminiLive.onSessionLost([]() { sessionLostFlag = true; });

  // Gemini streams the reply transcript in fragments; append them so the label
  // shows the whole sentence rather than the last few words.
  geminiLive.onSaying([](const char* text) {
    portENTER_CRITICAL(&replyMux);
    size_t used = strlen(pendingReply);
    strncat(pendingReply, text, sizeof(pendingReply) - used - 1);
    replyDirty = true;
    portEXIT_CRITICAL(&replyMux);
  });
  // Gemini streams the input transcript in fragments, same as the reply, so
  // they're appended rather than replacing each other.
  geminiLive.onHeard([](const char* text) {
    portENTER_CRITICAL(&replyMux);
    size_t used = strlen(pendingHeard);
    strncat(pendingHeard, text, sizeof(pendingHeard) - used - 1);
    heardDirty = true;
    portEXIT_CRITICAL(&replyMux);
  });

  lv_timer_create(GranVoice_Tick, 100, NULL);
}

void GranVoice_UI_ShowWifiSetup(void) {
  ShowWifiList();
}

#pragma once
#include <Arduino.h>
#include <functional>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class GeminiLiveClient {
public:
  using AudioCallback = std::function<void(const uint8_t* pcm, size_t len)>;
  using SimpleCallback = std::function<void()>;
  using TextCallback = std::function<void(const char* text)>;

  void begin();          // call once, after WiFi is connected
  void reconnect();      // tear down and redial (e.g. after switching WiFi network)
  void loop();           // pump frequently from a dedicated task
  bool isReady() const { return _ready; }

  void beginTurn();                                      // re-arms audio sending
  void sendAudioChunk(const uint8_t* pcm16, size_t len); // 16kHz, 16-bit, mono
  void sendAudioStreamEnd();

  void onAudio(AudioCallback cb) { _onAudio = cb; }
  void onTurnComplete(SimpleCallback cb) { _onTurnComplete = cb; }
  void onInterrupted(SimpleCallback cb) { _onInterrupted = cb; }
  void onHeard(TextCallback cb) { _onHeard = cb; }   // transcript of the user's speech
  void onSaying(TextCallback cb) { _onSaying = cb; } // transcript of Gemini's reply

  // Fired when the server drops the session unexpectedly (i.e. not because we
  // asked). Without this the UI just fell silent mid-answer, leaving no clue
  // that anything went wrong or that retrying would help.
  void onSessionLost(SimpleCallback cb) { _onSessionLost = cb; }

  // True once the server has refused a session with a quota error. Nothing will
  // work until the quota window resets, so the UI says so rather than looking broken.
  bool isQuotaExhausted() const { return _quotaExhausted; }

private:
  void handleEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleServerText(const char* json, size_t len);
  void sendSetup();

  // WebSocketsClient is NOT thread-safe, and three tasks reach it: GeminiLoopTask
  // (loop(), which tears down the TLS client on disconnect), GVCapture
  // (sendAudioChunk) and the LVGL task (sendAudioStreamEnd, via a cancel tap).
  // Tapping mid-turn while the socket was dropping freed the TLS object under
  // another task and crashed with InstrFetchProhibited at PC=0. Every entry
  // point below takes this lock.
  SemaphoreHandle_t _wsMutex = nullptr;

  WebSocketsClient _ws;
  bool _wsConnected = false;
  bool _ready = false; // setupComplete received
  bool _quotaExhausted = false;
  // Set once audioStreamEnd has gone out, cleared by beginTurn(). Sending audio
  // after the stream end is a protocol violation the server answers with
  // close 1007 ("audio content type not supported for this model
  // configuration"), which used to happen when a cancel tap raced a capture
  // chunk already past its own `capturing` check.
  bool _streamEnded = false;
  unsigned long _sessionStartMs = 0;

  String _fragBuf; // accumulates fragmented TEXT frames, if any arrive
  String _sessionHandle; // resumption token from the last sessionResumptionUpdate, if any

  AudioCallback _onAudio;
  SimpleCallback _onTurnComplete;
  SimpleCallback _onInterrupted;
  TextCallback _onHeard;
  TextCallback _onSaying;
  SimpleCallback _onSessionLost;
};

extern GeminiLiveClient geminiLive;

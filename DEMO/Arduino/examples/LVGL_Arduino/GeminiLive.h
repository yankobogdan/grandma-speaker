#pragma once
#include <Arduino.h>
#include <functional>
#include <WebSocketsClient.h>

class GeminiLiveClient {
public:
  using AudioCallback = std::function<void(const uint8_t* pcm, size_t len)>;
  using SimpleCallback = std::function<void()>;
  using TextCallback = std::function<void(const char* text)>;

  void begin();          // call once, after WiFi is connected
  void reconnect();      // tear down and redial (e.g. after switching WiFi network)
  void loop();           // pump frequently from a dedicated task
  bool isReady() const { return _ready; }

  void sendAudioChunk(const uint8_t* pcm16, size_t len); // 16kHz, 16-bit, mono
  void sendAudioStreamEnd();

  void onAudio(AudioCallback cb) { _onAudio = cb; }
  void onTurnComplete(SimpleCallback cb) { _onTurnComplete = cb; }
  void onInterrupted(SimpleCallback cb) { _onInterrupted = cb; }
  void onHeard(TextCallback cb) { _onHeard = cb; }   // transcript of the user's speech
  void onSaying(TextCallback cb) { _onSaying = cb; } // transcript of Gemini's reply

  // True once the server has refused a session with a quota error. Nothing will
  // work until the quota window resets, so the UI says so rather than looking broken.
  bool isQuotaExhausted() const { return _quotaExhausted; }

private:
  void handleEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleServerText(const char* json, size_t len);
  void sendSetup();

  WebSocketsClient _ws;
  bool _wsConnected = false;
  bool _ready = false; // setupComplete received
  bool _quotaExhausted = false;

  String _fragBuf; // accumulates fragmented TEXT frames, if any arrive
  String _sessionHandle; // resumption token from the last sessionResumptionUpdate, if any

  AudioCallback _onAudio;
  SimpleCallback _onTurnComplete;
  SimpleCallback _onInterrupted;
  TextCallback _onHeard;
  TextCallback _onSaying;
};

extern GeminiLiveClient geminiLive;

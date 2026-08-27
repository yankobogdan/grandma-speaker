#include "GeminiLive.h"
#include "GranVoice_Config.h"
#include <mbedtls/base64.h>
#include <ArduinoJson.h>

// Google Trust Services root bundle (R1-R4, from https://pki.goog/repo/certs/).
// generativelanguage.googleapis.com's edge nodes have been observed serving
// chains rooted at GTS Root R4 (ECC) rather than R1 (RSA) - Google spreads
// traffic across several parallel roots, so all four are bundled here instead
// of pinning just one. mbedtls_x509_crt_parse (used under NetworkClientSecure::
// setCACert) accepts a single buffer with multiple concatenated PEM blocks.
static const char GTS_ROOTS_PEM[] =
// GTS Root R1
"-----BEGIN CERTIFICATE-----\n"
"MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
"MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
"A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
"27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n"
"Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n"
"TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n"
"qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n"
"szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n"
"Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n"
"MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n"
"wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n"
"aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n"
"VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n"
"AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
"FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n"
"C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n"
"QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n"
"h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n"
"7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n"
"ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n"
"MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n"
"Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n"
"6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n"
"0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n"
"2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n"
"bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
"-----END CERTIFICATE-----\n"
// GTS Root R2
"-----BEGIN CERTIFICATE-----\n"
"MIIFVzCCAz+gAwIBAgINAgPlrsWNBCUaqxElqjANBgkqhkiG9w0BAQwFADBHMQsw\n"
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
"MBIGA1UEAxMLR1RTIFJvb3QgUjIwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjIwggIiMA0GCSqGSIb3DQEBAQUA\n"
"A4ICDwAwggIKAoICAQDO3v2m++zsFDQ8BwZabFn3GTXd98GdVarTzTukk3LvCvpt\n"
"nfbwhYBboUhSnznFt+4orO/LdmgUud+tAWyZH8QiHZ/+cnfgLFuv5AS/T3KgGjSY\n"
"6Dlo7JUle3ah5mm5hRm9iYz+re026nO8/4Piy33B0s5Ks40FnotJk9/BW9BuXvAu\n"
"MC6C/Pq8tBcKSOWIm8Wba96wyrQD8Nr0kLhlZPdcTK3ofmZemde4wj7I0BOdre7k\n"
"RXuJVfeKH2JShBKzwkCX44ofR5GmdFrS+LFjKBC4swm4VndAoiaYecb+3yXuPuWg\n"
"f9RhD1FLPD+M2uFwdNjCaKH5wQzpoeJ/u1U8dgbuak7MkogwTZq9TwtImoS1mKPV\n"
"+3PBV2HdKFZ1E66HjucMUQkQdYhMvI35ezzUIkgfKtzra7tEscszcTJGr61K8Yzo\n"
"dDqs5xoic4DSMPclQsciOzsSrZYuxsN2B6ogtzVJV+mSSeh2FnIxZyuWfoqjx5RW\n"
"Ir9qS34BIbIjMt/kmkRtWVtd9QCgHJvGeJeNkP+byKq0rxFROV7Z+2et1VsRnTKa\n"
"G73VululycslaVNVJ1zgyjbLiGH7HrfQy+4W+9OmTN6SpdTi3/UGVN4unUu0kzCq\n"
"gc7dGtxRcw1PcOnlthYhGXmy5okLdWTK1au8CcEYof/UVKGFPP0UJAOyh9OktwID\n"
"AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n"
"FgQUu//KjiOfT5nK2+JopqUVJxce2Q4wDQYJKoZIhvcNAQEMBQADggIBAB/Kzt3H\n"
"vqGf2SdMC9wXmBFqiN495nFWcrKeGk6c1SuYJF2ba3uwM4IJvd8lRuqYnrYb/oM8\n"
"0mJhwQTtzuDFycgTE1XnqGOtjHsB/ncw4c5omwX4Eu55MaBBRTUoCnGkJE+M3DyC\n"
"B19m3H0Q/gxhswWV7uGugQ+o+MePTagjAiZrHYNSVc61LwDKgEDg4XSsYPWHgJ2u\n"
"NmSRXbBoGOqKYcl3qJfEycel/FVL8/B/uWU9J2jQzGv6U53hkRrJXRqWbTKH7QMg\n"
"yALOWr7Z6v2yTcQvG99fevX4i8buMTolUVVnjWQye+mew4K6Ki3pHrTgSAai/Gev\n"
"HyICc/sgCq+dVEuhzf9gR7A/Xe8bVr2XIZYtCtFenTgCR2y59PYjJbigapordwj6\n"
"xLEokCZYCDzifqrXPW+6MYgKBesntaFJ7qBFVHvmJ2WZICGoo7z7GJa7Um8M7YNR\n"
"TOlZ4iBgxcJlkoKM8xAfDoqXvneCbT+PHV28SSe9zE8P4c52hgQjxcCMElv924Sg\n"
"JPFI/2R80L5cFtHvma3AH/vLrrw4IgYmZNralw4/KBVEqE8AyvCazM90arQ+POuV\n"
"7LXTWtiBmelDGDfrs7vRWGJB82bSj6p4lVQgw1oudCvV0b4YacCs1aTPObpRhANl\n"
"6WLAYv7YTVWW4tAR+kg0Eeye7QUd5MjWHYbL\n"
"-----END CERTIFICATE-----\n"
// GTS Root R3
"-----BEGIN CERTIFICATE-----\n"
"MIICCTCCAY6gAwIBAgINAgPluILrIPglJ209ZjAKBggqhkjOPQQDAzBHMQswCQYD\n"
"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
"A1UEAxMLR1RTIFJvb3QgUjMwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
"WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
"IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjMwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
"AAQfTzOHMymKoYTey8chWEGJ6ladK0uFxh1MJ7x/JlFyb+Kf1qPKzEUURout736G\n"
"jOyxfi//qXGdGIRFBEFVbivqJn+7kAHjSxm65FSWRQmx1WyRRK2EE46ajA2ADDL2\n"
"4CejQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
"BBTB8Sa6oC2uhYHP0/EqEr24Cmf9vDAKBggqhkjOPQQDAwNpADBmAjEA9uEglRR7\n"
"VKOQFhG/hMjqb2sXnh5GmCCbn9MN2azTL818+FsuVbu/3ZL3pAzcMeGiAjEA/Jdm\n"
"ZuVDFhOD3cffL74UOO0BzrEXGhF16b0DjyZ+hOXJYKaV11RZt+cRLInUue4X\n"
"-----END CERTIFICATE-----\n"
// GTS Root R4
"-----BEGIN CERTIFICATE-----\n"
"MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
"A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
"WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
"IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
"AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
"QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
"HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
"BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
"9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
"p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
"-----END CERTIFICATE-----\n";

static const char* GEMINI_HOST = "generativelanguage.googleapis.com";
static const uint16_t GEMINI_PORT = 443;

GeminiLiveClient geminiLive;

void GeminiLiveClient::begin() {
  Serial.printf("[GeminiLive] heap before connect: free=%u largest_internal_block=%u free_internal=%u free_spiram=%u\n",
    ESP.getFreeHeap(),
    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  String url = String("/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=") + GEMINI_API_KEY;

  _ws.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
    this->handleEvent(type, payload, length);
  });
  _ws.beginSslWithCA(GEMINI_HOST, GEMINI_PORT, url.c_str(), GTS_ROOTS_PEM, "");
  _ws.setReconnectInterval(3000);
}

void GeminiLiveClient::reconnect() {
  // The old socket belongs to the previous network; drop it before redialing.
  // _sessionHandle is deliberately kept, so the conversation resumes rather
  // than restarting just because the WiFi network changed.
  _ws.disconnect();
  _wsConnected = false;
  _ready = false;
  begin();
}

void GeminiLiveClient::loop() {
  _ws.loop();
}

void GeminiLiveClient::sendSetup() {
  String msg;
  msg.reserve(strlen(GRANVOICE_SYSTEM_PROMPT) + 300);
  msg += "{\"setup\":{\"model\":\"";
  msg += GEMINI_MODEL;
  msg += "\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],"
         "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"";
  msg += GEMINI_VOICE_NAME;
  msg += "\"}}}},\"systemInstruction\":{\"parts\":[{\"text\":\"";
  msg += GRANVOICE_SYSTEM_PROMPT;
  // Ask for text transcripts of both sides alongside the audio - lets us verify
  // over Serial what Gemini heard and what it's saying, without depending on the
  // speaker/mic being physically correct yet.
  msg += "\"}]},\"inputAudioTranscription\":{},\"outputAudioTranscription\":{},";
  // Google Search grounding. Bills against a quota that is separate from the
  // general request quota AND split by model generation: Gemini 3.x models
  // share one 5,000/month pool (exhausted on this account - a 3.6-flash REST
  // call with this tool returns 429 while the identical call without it
  // returns 200), whereas 2.5 models draw from a 1,500/day pool. GEMINI_MODEL
  // is a 2.5 native-audio model, so this should hit the daily pool instead.
  msg += "\"tools\":[{\"googleSearch\":{}}],";
  // Extends the session beyond the default ~15min audio-session cap.
  msg += "\"contextWindowCompression\":{\"slidingWindow\":{}},";
  // Resume the previous session (keeping conversation context) if we have a
  // handle from an earlier sessionResumptionUpdate; otherwise start fresh and
  // request one for next time.
  msg += "\"sessionResumption\":{";
  if (_sessionHandle.length() > 0) {
    msg += "\"handle\":\"" + _sessionHandle + "\"";
  }
  msg += "}}}";
  Serial.printf("[GeminiLive] sending setup (resuming: %s)\n", _sessionHandle.length() > 0 ? "yes" : "no");
  _ws.sendTXT(msg);
}

void GeminiLiveClient::sendAudioChunk(const uint8_t* pcm16, size_t len) {
  if (!_ready) return;

  // Internal RAM, not PSRAM: this runs on the hot path inside the WS callback,
  // and PSRAM's extra access latency was implicated in a connection-dropping
  // stall when processing back-to-back large audio-delta frames (see the
  // matching note on the parse-side buffers in handleServerText below).
  static const size_t B64BUF_CAP = 6000; // fits chunks up to ~4.4KB raw (well under the 15KB ws frame cap)
  static uint8_t* b64buf = (uint8_t*) malloc(B64BUF_CAP);
  size_t b64cap = 4 * ((len + 2) / 3) + 4;
  if (b64cap > B64BUF_CAP) {
    Serial.println("[GeminiLive] audio chunk too large, dropping");
    return;
  }
  size_t outLen = 0;
  if (mbedtls_base64_encode(b64buf, B64BUF_CAP, &outLen, pcm16, len) != 0) {
    return;
  }

  String msg;
  msg.reserve(outLen + 100);
  msg += "{\"realtimeInput\":{\"audio\":{\"data\":\"";
  msg.concat((const char*)b64buf, outLen);
  msg += "\",\"mimeType\":\"audio/pcm;rate=16000\"}}}";
  _ws.sendTXT(msg);
}

void GeminiLiveClient::sendAudioStreamEnd() {
  if (!_wsConnected) return;
  _ws.sendTXT("{\"realtimeInput\":{\"audioStreamEnd\":true}}");
}

void GeminiLiveClient::handleEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[GeminiLive] WSS connected");
      _wsConnected = true;
      _ready = false;
      _fragBuf = "";
      sendSetup();
      break;
    case WStype_DISCONNECTED:
      Serial.println("[GeminiLive] WSS disconnected");
      _wsConnected = false;
      _ready = false;
      // A quota refusal won't clear by retrying hard - hammering it every 3s
      // just adds load and risks deeper throttling. Back right off and let the
      // UI tell the user, rather than looking silently broken.
      if (strstr(ws_last_close_reason, "quota") || strstr(ws_last_close_reason, "billing")) {
        if (!_quotaExhausted) {
          Serial.println("[GeminiLive] QUOTA EXHAUSTED - backing off to 5 min retries");
        }
        _quotaExhausted = true;
        _ws.setReconnectInterval(300000); // 5 min
      }
      break;
    case WStype_TEXT:
    case WStype_BIN: // the Live API sends its JSON frames with a binary opcode
      Serial.printf("[GeminiLive] RX frame: %u bytes\n", (unsigned)length);
      handleServerText((const char*)payload, length);
      break;
    case WStype_FRAGMENT_TEXT_START:
      Serial.printf("[GeminiLive] RX fragment start: %u bytes\n", (unsigned)length);
      _fragBuf = String((const char*)payload).substring(0, length);
      break;
    case WStype_FRAGMENT:
      Serial.printf("[GeminiLive] RX fragment: %u bytes\n", (unsigned)length);
      _fragBuf.concat((const char*)payload, length);
      break;
    case WStype_FRAGMENT_FIN:
      Serial.printf("[GeminiLive] RX fragment fin: %u bytes (total %u)\n", (unsigned)length, (unsigned)(_fragBuf.length() + length));
      _fragBuf.concat((const char*)payload, length);
      handleServerText(_fragBuf.c_str(), _fragBuf.length());
      _fragBuf = "";
      break;
    case WStype_ERROR:
      Serial.printf("[GeminiLive] WS error: %.*s\n", (int)length, (const char*)payload);
      break;
    default:
      break;
  }
}

void GeminiLiveClient::handleServerText(const char* json, size_t len) {
  // Internal RAM (not PSRAM) deliberately: this is the one buffer directly
  // implicated in a connection-dropping stall earlier - JSON DOM parsing is a
  // pointer-chasing, many-small-accesses workload, exactly what PSRAM's extra
  // access latency hurts most. We have internal headroom (~220KB) to spare for
  // it; everything else in this file (decode scratch, playback queue) is PSRAM.
  DynamicJsonDocument doc(98304); // audio deltas can be up to ~64KB of base64 (WEBSOCKETS_MAX_DATA_SIZE)
  DeserializationError err = deserializeJson(doc, json, len);
  if (err) {
    Serial.printf("[GeminiLive] JSON parse error: %s\n", err.c_str());
    return;
  }

  if (doc.containsKey("setupComplete")) {
    Serial.println("[GeminiLive] setup complete, ready");
    _ready = true;
    if (_quotaExhausted) { // quota window recovered
      _quotaExhausted = false;
      _ws.setReconnectInterval(3000);
      ws_last_close_reason[0] = '\0';
    }
    return;
  }

  JsonObject resumptionUpdate = doc["sessionResumptionUpdate"];
  if (!resumptionUpdate.isNull() && (resumptionUpdate["resumable"] | false)) {
    const char* newHandle = resumptionUpdate["newHandle"] | (const char*)nullptr;
    if (newHandle) {
      _sessionHandle = newHandle;
      Serial.println("[GeminiLive] session handle updated");
    }
  }

  JsonObject serverContent = doc["serverContent"];
  if (serverContent.isNull()) return;

  const char* heard = serverContent["inputTranscription"]["text"] | (const char*)nullptr;
  if (heard && heard[0]) {
    Serial.printf("[GeminiLive] heard: %s\n", heard);
    if (_onHeard) _onHeard(heard);
  }
  const char* saying = serverContent["outputTranscription"]["text"] | (const char*)nullptr;
  if (saying && saying[0]) {
    Serial.printf("[GeminiLive] saying: %s\n", saying);
    if (_onSaying) _onSaying(saying);
  }

  JsonObject modelTurn = serverContent["modelTurn"];
  if (!modelTurn.isNull()) {
    // PSRAM: base64 decode is a tight sequential scan (not pointer-chasing like
    // JSON DOM parsing above), so PSRAM's latency isn't the same risk here -
    // we have 8MB of it and use of internal RAM stays reserved for the doc.
    static const size_t PCM_BUF_CAP = 51200; // decoded size of a ~64KB base64 payload
    static uint8_t* pcmBuf = (uint8_t*) heap_caps_malloc(PCM_BUF_CAP, MALLOC_CAP_SPIRAM);
    for (JsonObject part : modelTurn["parts"].as<JsonArray>()) {
      JsonObject inlineData = part["inlineData"];
      const char* b64 = inlineData["data"] | (const char*)nullptr;
      if (b64 && _onAudio) {
        size_t b64len = strlen(b64);
        size_t outLen = 0;
        int rc = mbedtls_base64_decode(pcmBuf, PCM_BUF_CAP, &outLen, (const unsigned char*)b64, b64len);
        if (rc == 0) {
          _onAudio(pcmBuf, outLen);
        } else {
          Serial.printf("[GeminiLive] base64 decode error: %d\n", rc);
        }
      }
    }
  }

  if (serverContent["interrupted"] | false) {
    if (_onInterrupted) _onInterrupted();
  }
  if (serverContent["turnComplete"] | false) {
    if (_onTurnComplete) _onTurnComplete();
  }
}

#include "GranVoice_Audio.h"
#include "MIC_MSM.h"
#include "GeminiLive.h"
#include "ES8311.h"
#include <freertos/stream_buffer.h>
#include <freertos/semphr.h>
#include <Preferences.h>

// Mic capture: 100ms chunks at 16kHz mono (matches GeminiLive's b64 buffer sizing
// and the shared bus's AUDIO_SAMPLE_RATE).
#define CAPTURE_CHUNK_SAMPLES 1600

// Gemini Live always sends reply audio at 24kHz mono 16-bit PCM, but the shared
// codec bus on this board (mic + speaker together) runs at 16kHz - resampled
// below rather than run two clock domains on hardware that only has one.
#define GEMINI_AUDIO_RATE 24000

static StreamBufferHandle_t playbackStream = nullptr;
static StaticStreamBuffer_t playbackStreamStruct; // small control block, fine in internal RAM
static volatile bool capturing = false;

// Serializes the two writers to the single TX channel (reply playback and the
// UI tap click). RX is deliberately NOT covered - see PlaybackTask.
static SemaphoreHandle_t i2sTxMutex = nullptr;

// A single decoded audio-delta chunk can now be up to ~50KB (see GeminiLive.cpp's
// PCM_BUF_CAP, sized for the raised WEBSOCKETS_MAX_DATA_SIZE) - sized for several
// chunks of headroom. PSRAM-backed (see GranVoice_Audio_Init), so this is cheap:
// out of 8MB PSRAM, not the scarce ~220KB of internal RAM.
#define PLAYBACK_STREAM_BYTES (192 * 1024)

static void PlaybackTask(void*) {
  // int16 samples in (Gemini's 24kHz PCM) -> linearly resampled to 16kHz -> each
  // sample widened to a left-justified int32 slot for the codec's 32-bit format.
  //
  // Larger chunks than before (85ms rather than 21ms per pass): fewer, bigger
  // I2S writes leave far more slack before the TX DMA runs dry, which is what
  // was truncating replies into noise.
  static const size_t IN_BUF_SAMPLES = 2048;
  static int16_t* inBuf = (int16_t*) heap_caps_malloc((IN_BUF_SAMPLES + 1) * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  static int32_t* outBuf = (int32_t*) heap_caps_malloc((IN_BUF_SAMPLES + 8) * sizeof(int32_t), MALLOC_CAP_SPIRAM);
  static double srcPos = 0.0;
  static int16_t carry = 0;      // last sample of the previous chunk
  static bool haveCarry = false; // false until the first chunk has been seen
  static const double RESAMPLE_STEP = (double)GEMINI_AUDIO_RATE / AUDIO_SAMPLE_RATE;

  if (!inBuf || !outBuf) {
    Serial.println("[Playback] FATAL: buffer alloc failed");
    vTaskDelete(NULL);
    return;
  }

  for (;;) {
    // inBuf[0] holds the carry sample so interpolation spans the chunk seam;
    // without it every chunk boundary dropped a sample and clicked.
    size_t n = xStreamBufferReceive(playbackStream, (uint8_t*)(inBuf + 1),
                                    IN_BUF_SAMPLES * sizeof(int16_t), pdMS_TO_TICKS(200));
    if (n == 0) {
      haveCarry = false; // idle gap - restart cleanly on the next reply
      srcPos = 0.0;
      continue;
    }
    size_t got = n / sizeof(int16_t);

    size_t first, avail;
    if (haveCarry) {
      inBuf[0] = carry;
      first = 0;
      avail = got + 1;
    } else {
      first = 1;
      avail = got + 1;
      srcPos = 1.0; // skip the unused slot 0
    }

    size_t outCount = 0;
    while (true) {
      size_t idx = (size_t)srcPos;
      if (idx + 1 >= avail) break;
      double frac = srcPos - (double)idx;
      int16_t s0 = inBuf[idx];
      int16_t s1 = inBuf[idx + 1];
      outBuf[outCount++] = ((int32_t)(int16_t)(s0 + (int32_t)((s1 - s0) * frac))) << 16;
      srcPos += RESAMPLE_STEP;
    }
    (void)first;

    // Carry the final input sample and rebase srcPos relative to it, so the
    // next chunk continues the waveform seamlessly.
    carry = inBuf[avail - 1];
    haveCarry = true;
    srcPos -= (double)(avail - 1);
    if (srcPos < 0) srcPos = 0;

    if (outCount > 0) {
      // TX-only lock (shared just with the tap click). It deliberately does NOT
      // cover CaptureTask's read: RX and TX are independent DMA channels, and
      // holding one lock across that blocking 100ms mic read was stalling
      // playback long enough to underrun TX - the truncation-into-noise symptom.
      uint32_t t0 = millis();
      xSemaphoreTake(i2sTxMutex, portMAX_DELAY);
      size_t wrote = i2s.write((uint8_t*)outBuf, outCount * sizeof(int32_t));
      xSemaphoreGive(i2sTxMutex);
      uint32_t dt = millis() - t0;

      static unsigned long lastPlayLog = 0;
      if (millis() - lastPlayLog > 1000) {
        lastPlayLog = millis();
        Serial.printf("[Playback] out=%u samples, wrote=%u B in %ums, queued=%u B\n",
                      (unsigned)outCount, (unsigned)wrote, (unsigned)dt,
                      (unsigned)(PLAYBACK_STREAM_BYTES - xStreamBufferSpacesAvailable(playbackStream)));
      }
    }
  }
}

static void CaptureTask(void*) {
  // PSRAM-backed: internal DRAM is the scarce resource mbedtls/I2S/SPI-DMA compete for.
  static const size_t RAW_BUF_BYTES = CAPTURE_CHUNK_SAMPLES * sizeof(int32_t);
  static int32_t* rawBuf = (int32_t*) heap_caps_malloc(RAW_BUF_BYTES, MALLOC_CAP_SPIRAM);
  static int16_t* monoBuf = (int16_t*) heap_caps_malloc(CAPTURE_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  for (;;) {
    if (!capturing) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    // Unsynchronised on purpose - see the note in PlaybackTask. This read blocks
    // for ~100ms waiting on mic DMA, and holding a shared lock across it was
    // starving playback.
    size_t got = i2s.readBytes((char*)rawBuf, RAW_BUF_BYTES);
    size_t gotSamples = got / sizeof(int32_t);
    // ES7210 outputs left-justified 32-bit words (MSB-aligned) - the top 16 bits
    // are the meaningful sample, unlike the old raw-MEMS-mic ">>14" scaling hack
    // that only applied to V1's direct-wired analog mic, not this digital codec.
    int16_t peak = 0;
    for (size_t i = 0; i < gotSamples; i++) {
      int16_t v = (int16_t)(rawBuf[i] >> 16);
      monoBuf[i] = v;
      int16_t av = v < 0 ? -v : v;
      if (av > peak) peak = av;
    }
    static unsigned long lastLevelPrint = 0;
    if (millis() - lastLevelPrint > 500) {
      lastLevelPrint = millis();
      Serial.printf("[GranVoice] mic peak: %d / 32767\n", peak);
    }
    if (gotSamples > 0 && capturing) {
      geminiLive.sendAudioChunk((const uint8_t*)monoBuf, gotSamples * sizeof(int16_t));
    }
  }
}

void GranVoice_Audio_Init() {
  i2sTxMutex = xSemaphoreCreateMutex();

  // Storage array must be xBufferSizeBytes + 1 (FreeRTOS stream buffer requirement).
  uint8_t* playbackStreamStorage = (uint8_t*) heap_caps_malloc(PLAYBACK_STREAM_BYTES + 1, MALLOC_CAP_SPIRAM);
  playbackStream = xStreamBufferCreateStatic(PLAYBACK_STREAM_BYTES, 1, playbackStreamStorage, &playbackStreamStruct);

  // Playback outranks capture: an underfed TX DMA is audible immediately (the
  // reply breaks up into noise), whereas a late mic read just delays a chunk.
  xTaskCreatePinnedToCore(PlaybackTask, "GVPlayback", 4096, NULL, 6, NULL, 0);
  xTaskCreatePinnedToCore(CaptureTask, "GVCapture", 4096, NULL, 4, NULL, 0);

  GranVoice_Audio_SetVolume(GranVoice_Audio_GetVolume()); // re-apply the saved level
}

void GranVoice_Audio_StartCapture() {
  capturing = true;
}

void GranVoice_Audio_StopCapture() {
  capturing = false;
}

void GranVoice_Audio_QueuePlayback(const uint8_t* pcm24k, size_t len) {
  if (!GranVoice_Audio_GetSpeechEnabled()) return; // text-only reply mode
  if (playbackStream) {
    xStreamBufferSend(playbackStream, pcm24k, len, pdMS_TO_TICKS(100));
  }
}

bool GranVoice_Audio_IsPlaybackIdle() {
  return playbackStream == nullptr || xStreamBufferIsEmpty(playbackStream);
}

void GranVoice_Audio_FlushPlayback() {
  if (playbackStream) {
    xStreamBufferReset(playbackStream);
  }
}

static int speechEnabled = -1; // -1 = not yet loaded from NVS

bool GranVoice_Audio_GetSpeechEnabled() {
  if (speechEnabled < 0) {
    Preferences p;
    p.begin("granvoice", true);
    speechEnabled = p.getBool("speech", true) ? 1 : 0;
    p.end();
  }
  return speechEnabled == 1;
}

void GranVoice_Audio_SetSpeechEnabled(bool enabled) {
  speechEnabled = enabled ? 1 : 0;
  if (!enabled) GranVoice_Audio_FlushPlayback();
  Preferences p;
  p.begin("granvoice", false);
  p.putBool("speech", enabled);
  p.end();
}

// Short click for UI feedback, written straight to I2S rather than through the
// playback queue so it stays snappy and never mixes into a spoken reply.
void GranVoice_Audio_PlayTap() {
  static const int TAP_MS = 18;
  static const int TAP_HZ = 1800;
  const size_t samples = (AUDIO_SAMPLE_RATE * TAP_MS) / 1000;
  static int32_t* tap = nullptr;
  if (!tap) {
    tap = (int32_t*) heap_caps_malloc(samples * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!tap) return;
    for (size_t i = 0; i < samples; i++) {
      float t = (float)i / (float)AUDIO_SAMPLE_RATE;
      // Fade out over the click so it ends without an audible pop.
      float env = 1.0f - ((float)i / (float)samples);
      int16_t s = (int16_t)(6000.0f * env * sinf(2.0f * PI * TAP_HZ * t));
      tap[i] = ((int32_t)s) << 16;
    }
  }
  if (!i2sTxMutex) return;
  // Skip the click rather than make the UI wait on an in-flight reply write.
  if (xSemaphoreTake(i2sTxMutex, pdMS_TO_TICKS(40)) != pdTRUE) return;
  i2s.write((uint8_t*)tap, samples * sizeof(int32_t));
  xSemaphoreGive(i2sTxMutex);
}

static int currentVolume = -1;

int GranVoice_Audio_GetVolume() {
  if (currentVolume < 0) {
    Preferences p;
    p.begin("granvoice", true);
    currentVolume = p.getInt("volume", 80);
    p.end();
  }
  return currentVolume;
}

void GranVoice_Audio_SetVolume(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  currentVolume = percent;
  ES8311_SetVolume(percent);
  Preferences p;
  p.begin("granvoice", false);
  p.putInt("volume", percent);
  p.end();
}

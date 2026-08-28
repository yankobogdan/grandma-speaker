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

// Gemini streams a reply considerably faster than real time, so this has to
// hold an entire answer, not just smooth out jitter. At 192KB (~4s of 24kHz
// audio) the buffer filled during any longer reply and xStreamBufferSend
// silently discarded whatever didn't fit - dropouts heard as chunks of speech
// separated by silence. 3MB is ~65s of audio and still a small slice of the
// 8MB PSRAM.
#define PLAYBACK_STREAM_BYTES (3 * 1024 * 1024)

// Playback must start only once enough audio is buffered to ride out network
// jitter. Gemini's deltas arrive unevenly, and starting on the first byte left
// the DMA starved between packets - the "chunk by chunk" stutter.
#define PREROLL_MS 700
#define PREROLL_BYTES ((GEMINI_AUDIO_RATE * 2 * PREROLL_MS) / 1000)

// 24000 -> 16000 is exactly 3:2, so the step is 1.5 in 16.16 fixed point.
// Fixed point rather than double on purpose: the ESP32-S3 FPU is single
// precision only, so double maths is software-emulated and doing several
// operations per sample at 16kHz was itself burning enough CPU to cause
// underruns.
#define RESAMPLE_STEP_Q16 ((uint32_t)(0x18000))

static volatile bool playbackActive = false;  // false until pre-roll is satisfied
static volatile bool replyStreaming = false;  // true from first audio until turnComplete

void GranVoice_Audio_ReplyBegin() { replyStreaming = true; }
void GranVoice_Audio_ReplyEnd()   { replyStreaming = false; }

static void PlaybackTask(void*) {
  static const size_t IN_BUF_SAMPLES = 4096;
  // Internal RAM, not PSRAM: the resampler touches these per sample and PSRAM
  // access is markedly slower. ~24KB out of ~220KB free, and it keeps the I2S
  // DMA source in fast memory too.
  static int16_t* inBuf = (int16_t*) heap_caps_malloc((IN_BUF_SAMPLES + 2) * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  static int32_t* outBuf = (int32_t*) heap_caps_malloc((IN_BUF_SAMPLES + 8) * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  static uint32_t srcPosQ16 = 0;
  static int16_t carry = 0;
  static bool haveCarry = false;

  if (!inBuf || !outBuf) {
    Serial.println("[Playback] FATAL: buffer alloc failed");
    vTaskDelete(NULL);
    return;
  }

  unsigned long lastGrowthMs = 0;
  size_t lastAvail = 0;

  for (;;) {
    // --- pre-roll gate -------------------------------------------------------
    if (!playbackActive) {
      size_t avail = xStreamBufferBytesAvailable(playbackStream);
      if (avail == 0) {
        haveCarry = false;
        srcPosQ16 = 0;
        lastAvail = 0;
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if (avail > lastAvail) {          // still filling
        lastAvail = avail;
        lastGrowthMs = millis();
      }
      // Start once the cushion is built, or immediately if the reply has
      // already finished (a short answer that will never reach PREROLL_BYTES).
      // The old "stopped growing for 250ms" guess fired on ordinary network
      // gaps and started playback with almost nothing buffered.
      bool cushioned = avail >= (size_t)PREROLL_BYTES;
      bool replyOver = !replyStreaming;
      if (!cushioned && !replyOver) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      playbackActive = true;
      Serial.printf("[Playback] start (buffered %u B, %s)\n",
                    (unsigned)avail, cushioned ? "cushion" : "reply-complete");
    }

    // --- greedy fill ---------------------------------------------------------
    // One blocking read, then top up without blocking, so each I2S write is as
    // large as possible instead of a few milliseconds at a time.
    size_t got = 0;
    size_t n = xStreamBufferReceive(playbackStream, (uint8_t*)(inBuf + 1),
                                    IN_BUF_SAMPLES * sizeof(int16_t), pdMS_TO_TICKS(120));
    got = n / sizeof(int16_t);
    while (got < IN_BUF_SAMPLES) {
      size_t more = xStreamBufferReceive(playbackStream,
                                         (uint8_t*)(inBuf + 1 + got),
                                         (IN_BUF_SAMPLES - got) * sizeof(int16_t), 0);
      if (more == 0) break;
      got += more / sizeof(int16_t);
    }

    if (got == 0) {
      if (replyStreaming) {
        // Underrun mid-answer: more audio is still coming, so hold the playback
        // state and wait rather than re-running the pre-roll, which is what
        // chopped answers into bursts separated by silence.
        static unsigned long lastStarveLog = 0;
        if (millis() - lastStarveLog > 1000) {
          lastStarveLog = millis();
          Serial.println("[Playback] starved mid-reply - waiting for more audio");
        }
        continue;
      }
      // Reply finished and everything drained: re-arm for the next one.
      playbackActive = false;
      haveCarry = false;
      srcPosQ16 = 0;
      lastAvail = 0;
      continue;
    }

    // --- resample 24k -> 16k, fixed point ------------------------------------
    size_t avail;
    if (haveCarry) {
      inBuf[0] = carry;
      avail = got + 1;
    } else {
      avail = got + 1;
      srcPosQ16 = 1u << 16; // skip the unused slot 0
    }

    size_t outCount = 0;
    for (;;) {
      uint32_t idx = srcPosQ16 >> 16;
      if (idx + 1 >= avail) break;
      uint32_t frac = srcPosQ16 & 0xFFFF;
      int32_t s0 = inBuf[idx];
      int32_t s1 = inBuf[idx + 1];
      int32_t v = s0 + (((s1 - s0) * (int32_t)frac) >> 16);
      outBuf[outCount++] = v << 16;   // left-justify into the codec's 32-bit slot
      srcPosQ16 += RESAMPLE_STEP_Q16;
    }

    carry = inBuf[avail - 1];
    haveCarry = true;
    srcPosQ16 -= (uint32_t)((avail - 1) << 16);

    if (outCount > 0) {
      uint32_t t0 = millis();
      xSemaphoreTake(i2sTxMutex, portMAX_DELAY);
      size_t wrote = i2s.write((uint8_t*)outBuf, outCount * sizeof(int32_t));
      xSemaphoreGive(i2sTxMutex);
      uint32_t dt = millis() - t0;

      static unsigned long lastPlayLog = 0;
      if (millis() - lastPlayLog > 1000) {
        lastPlayLog = millis();
        Serial.printf("[Playback] out=%u samples (%ums audio), wrote=%u B in %ums, queued=%u B\n",
                      (unsigned)outCount, (unsigned)(outCount * 1000 / AUDIO_SAMPLE_RATE),
                      (unsigned)wrote, (unsigned)dt,
                      (unsigned)xStreamBufferBytesAvailable(playbackStream));
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
  xTaskCreatePinnedToCore(PlaybackTask, "GVPlayback", 4096, NULL, 12, NULL, 0);
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
  if (!playbackStream) return;
  size_t sent = xStreamBufferSend(playbackStream, pcm24k, len, pdMS_TO_TICKS(100));
  if (sent < len) {
    // Loud, because it means the listener lost part of the answer.
    Serial.printf("[Playback] BUFFER FULL - dropped %u of %u bytes (queued %u)\n",
                  (unsigned)(len - sent), (unsigned)len,
                  (unsigned)(PLAYBACK_STREAM_BYTES - xStreamBufferSpacesAvailable(playbackStream)));
  }
}

bool GranVoice_Audio_IsPlaybackIdle() {
  // Also false while a reply is mid-flight: with the pre-roll gate the stream
  // buffer can drain momentarily while audio is still being played out, and
  // treating that as "idle" ended the turn early.
  if (playbackActive) return false;
  return playbackStream == nullptr || xStreamBufferIsEmpty(playbackStream);
}

void GranVoice_Audio_FlushPlayback() {
  if (playbackStream) {
    xStreamBufferReset(playbackStream);
  }
  playbackActive = false; // next reply re-buffers before starting
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

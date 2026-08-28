#pragma once
#include <Arduino.h>

void GranVoice_Audio_Init();          // sets up speaker I2S + playback task, call once in setup()
void GranVoice_Audio_StartCapture();  // start streaming mic audio -> Gemini
void GranVoice_Audio_StopCapture();   // stop streaming mic audio
void GranVoice_Audio_QueuePlayback(const uint8_t* pcm24k, size_t len); // feed decoded audio-out chunks
bool GranVoice_Audio_IsPlaybackIdle();
void GranVoice_Audio_FlushPlayback(); // discard any buffered playback (barge-in / cancel)

void GranVoice_Audio_SetVolume(int percent); // 0-100, persisted to NVS
int GranVoice_Audio_GetVolume();

// Spoken replies on/off (text is always shown). Persisted to NVS.
void GranVoice_Audio_SetSpeechEnabled(bool enabled);
bool GranVoice_Audio_GetSpeechEnabled();

void GranVoice_Audio_PlayTap(); // short click, UI tap feedback

// Reply lifecycle. Without this the playback task had to infer the end of a
// reply from an empty buffer, which is indistinguishable from a network gap:
// it would tear down and re-buffer mid-answer, producing short bursts of audio
// separated by silence.
void GranVoice_Audio_ReplyBegin();
void GranVoice_Audio_ReplyEnd();

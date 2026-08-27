#pragma once
#include <Arduino.h>

// Minimal driver for the ES8311 speaker DAC/codec on ESP32-S3-Touch-LCD-1.85C V2
// boards. Ported from Espressif's official component (github.com/espressif/esp-bsp,
// components/es8311), fixed to this project's exact config: 16kHz, 32-bit, I2S
// slave mode with MCLK derived from the shared bus's BCLK (no dedicated MCLK
// pin needed on this chip's side).

bool ES8311_Init();
bool ES8311_SetVolume(int volumePercent); // 0-100

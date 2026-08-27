#pragma once
#include <Arduino.h>

// Minimal driver for the ES7210 mic ADC/codec on ESP32-S3-Touch-LCD-1.85C V2
// boards. Ported from Espressif's official component (github.com/espressif/esp-bsp,
// components/es7210), fixed to this project's exact config: 16kHz, 32-bit I2S
// output, 256x MCLK ratio (4.096MHz, expected on the shared bus's MCLK pin).

bool ES7210_Init();

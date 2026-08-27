#pragma once
#include "ESP_I2S.h"

// ESP32-S3-Touch-LCD-1.85C V2: mic (ES7210) and speaker (ES8311) share ONE
// full-duplex I2S bus (confirmed against Waveshare's official V1/V2 pin diff
// table - GPIO2 is I2S_MCLK, not a standalone mic pin, on V2). Both codecs are
// I2C-configured (see ES7210.h/ES8311.h) and driven from this single peripheral.
#define AUDIO_BUS_BCLK 48
#define AUDIO_BUS_WS   38
#define AUDIO_BUS_DOUT 47 // to ES8311 (speaker)
#define AUDIO_BUS_DIN  39 // from ES7210 (mic)
#define AUDIO_BUS_MCLK 2  // required by ES7210; ES8311 derives its clock from BCLK instead

// Amplifier enable, GPIO only - not part of the I2S protocol.
#define PA_CTRL_PIN 15

#define AUDIO_SAMPLE_RATE 16000

extern I2SClass i2s;

void MIC_Init(void);

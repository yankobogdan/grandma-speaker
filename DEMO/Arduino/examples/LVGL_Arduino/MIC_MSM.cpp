#include "MIC_MSM.h"
#include "ES8311.h"
#include "ES7210.h"

I2SClass i2s;

void MIC_Init() {
  pinMode(PA_CTRL_PIN, OUTPUT);
  digitalWrite(PA_CTRL_PIN, HIGH); // enable the speaker amplifier

  i2s.setPins(AUDIO_BUS_BCLK, AUDIO_BUS_WS, AUDIO_BUS_DOUT, AUDIO_BUS_DIN, AUDIO_BUS_MCLK);
  i2s.setTimeout(1000);
  bool i2sOk = i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
  Serial.printf("[Audio] shared I2S bus begin() -> %s\n", i2sOk ? "OK" : "FAILED");

  bool es8311Ok = ES8311_Init();
  Serial.printf("[Audio] ES8311 (speaker) init -> %s\n", es8311Ok ? "OK" : "FAILED");

  bool es7210Ok = ES7210_Init();
  Serial.printf("[Audio] ES7210 (mic) init -> %s\n", es7210Ok ? "OK" : "FAILED");
}

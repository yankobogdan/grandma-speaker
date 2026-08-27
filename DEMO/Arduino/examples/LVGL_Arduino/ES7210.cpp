#include "ES7210.h"
#include "I2C_Driver.h"

#define ES7210_ADDR 0x40 // AD0/AD1 tied low (this board's default)

#define ES7210_RESET_REG00         0x00
#define ES7210_MAINCLK_REG02       0x02
#define ES7210_LRCK_DIVH_REG04     0x04
#define ES7210_LRCK_DIVL_REG05     0x05
#define ES7210_POWER_DOWN_REG06    0x06
#define ES7210_OSR_REG07           0x07
#define ES7210_TIME_CONTROL0_REG09 0x09
#define ES7210_TIME_CONTROL1_REG0A 0x0A
#define ES7210_SDP_INTERFACE1_REG11 0x11
#define ES7210_SDP_INTERFACE2_REG12 0x12
#define ES7210_ADC34_HPF2_REG20    0x20
#define ES7210_ADC34_HPF1_REG21    0x21
#define ES7210_ADC12_HPF2_REG22    0x22
#define ES7210_ADC12_HPF1_REG23    0x23
#define ES7210_ANALOG_REG40        0x40
#define ES7210_MIC12_BIAS_REG41    0x41
#define ES7210_MIC34_BIAS_REG42    0x42
#define ES7210_MIC1_GAIN_REG43     0x43
#define ES7210_MIC2_GAIN_REG44     0x44
#define ES7210_MIC3_GAIN_REG45     0x45
#define ES7210_MIC4_GAIN_REG46     0x46
#define ES7210_MIC1_POWER_REG47    0x47
#define ES7210_MIC2_POWER_REG48    0x48
#define ES7210_MIC3_POWER_REG49    0x49
#define ES7210_MIC4_POWER_REG4A    0x4A
#define ES7210_MIC12_POWER_REG4B   0x4B
#define ES7210_MIC34_POWER_REG4C   0x4C

static bool w(uint8_t reg, uint8_t val) {
  return !I2C_Write(ES7210_ADDR, reg, &val, 1); // I2C_Write returns true on FAILURE
}

bool ES7210_Init() {
  bool ok = true;

  // Software reset
  ok &= w(ES7210_RESET_REG00, 0xFF);
  ok &= w(ES7210_RESET_REG00, 0x32);
  ok &= w(ES7210_TIME_CONTROL0_REG09, 0x30);
  ok &= w(ES7210_TIME_CONTROL1_REG0A, 0x30);

  // High-pass filters for all 4 ADC channels
  ok &= w(ES7210_ADC12_HPF1_REG23, 0x2A);
  ok &= w(ES7210_ADC12_HPF2_REG22, 0x0A);
  ok &= w(ES7210_ADC34_HPF1_REG21, 0x2A);
  ok &= w(ES7210_ADC34_HPF2_REG20, 0x0A);

  // Format: standard I2S, 32-bit, no TDM
  ok &= w(ES7210_SDP_INTERFACE1_REG11, 0x00 | 0x80); // I2S fmt | 32-bit
  ok &= w(ES7210_SDP_INTERFACE2_REG12, 0x00);

  // Analog power, mic bias, mic gain (30dB - matches the vendor default)
  ok &= w(ES7210_ANALOG_REG40, 0xC3);
  ok &= w(ES7210_MIC12_BIAS_REG41, 0x70); // 2.87V bias
  ok &= w(ES7210_MIC34_BIAS_REG42, 0x70);
  uint8_t gain30db = 10 | 0x10;
  ok &= w(ES7210_MIC1_GAIN_REG43, gain30db);
  ok &= w(ES7210_MIC2_GAIN_REG44, gain30db);
  ok &= w(ES7210_MIC3_GAIN_REG45, gain30db);
  ok &= w(ES7210_MIC4_GAIN_REG46, gain30db);

  ok &= w(ES7210_MIC1_POWER_REG47, 0x08);
  ok &= w(ES7210_MIC2_POWER_REG48, 0x08);
  ok &= w(ES7210_MIC3_POWER_REG49, 0x08);
  ok &= w(ES7210_MIC4_POWER_REG4A, 0x08);

  // Sample rate 16kHz @ 256x MCLK ratio (4.096MHz), from the vendor driver's
  // coefficient table row {mclk=4096000, lrck=16000}: osr=0x20, adc_div=1,
  // dll=1, doubler=1, lrck_h=1, lrck_l=0.
  ok &= w(ES7210_OSR_REG07, 0x20);
  ok &= w(ES7210_MAINCLK_REG02, 0x01 | (0x01 << 6) | (0x01 << 7)); // adc_div | doubler<<6 | dll<<7
  ok &= w(ES7210_LRCK_DIVH_REG04, 0x01);
  ok &= w(ES7210_LRCK_DIVL_REG05, 0x00);

  ok &= w(ES7210_POWER_DOWN_REG06, 0x04); // power down DLL
  ok &= w(ES7210_MIC12_POWER_REG4B, 0x0F); // power on mic bias, ADC, PGA
  ok &= w(ES7210_MIC34_POWER_REG4C, 0x0F);

  // Enable device
  ok &= w(ES7210_RESET_REG00, 0x71);
  ok &= w(ES7210_RESET_REG00, 0x41);

  return ok;
}

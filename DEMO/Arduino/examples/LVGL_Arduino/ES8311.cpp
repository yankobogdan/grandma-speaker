#include "ES8311.h"
#include "I2C_Driver.h"

#define ES8311_ADDR 0x18 // CE pin low (this board's default)

#define ES8311_RESET_REG00   0x00
#define ES8311_CLK_MGR_REG01 0x01
#define ES8311_CLK_MGR_REG02 0x02
#define ES8311_CLK_MGR_REG03 0x03
#define ES8311_CLK_MGR_REG04 0x04
#define ES8311_CLK_MGR_REG05 0x05
#define ES8311_CLK_MGR_REG06 0x06
#define ES8311_CLK_MGR_REG07 0x07
#define ES8311_CLK_MGR_REG08 0x08
#define ES8311_SDPIN_REG09   0x09
#define ES8311_SDPOUT_REG0A  0x0A
#define ES8311_SYSTEM_REG0D  0x0D
#define ES8311_SYSTEM_REG0E  0x0E
#define ES8311_SYSTEM_REG12  0x12
#define ES8311_SYSTEM_REG13  0x13
#define ES8311_ADC_REG1C     0x1C
#define ES8311_DAC_REG31     0x31
#define ES8311_DAC_REG32     0x32
#define ES8311_DAC_REG37     0x37

static bool w(uint8_t reg, uint8_t val) {
  return !I2C_Write(ES8311_ADDR, reg, &val, 1); // I2C_Write returns true on FAILURE
}
static uint8_t r(uint8_t reg) {
  uint8_t v = 0;
  I2C_Read(ES8311_ADDR, reg, &v, 1);
  return v;
}

bool ES8311_Init() {
  bool ok = true;

  // Reset, then power-on command
  ok &= w(ES8311_RESET_REG00, 0x1F);
  delay(20);
  ok &= w(ES8311_RESET_REG00, 0x00);
  ok &= w(ES8311_RESET_REG00, 0x80);

  // Clock config: MCLK taken from the SCLK/BCLK pin (not a dedicated MCLK pin),
  // computed as sample_rate * resolution * 2 = 16000 * 32 * 2 = 1,024,000 Hz.
  // Coefficients below are the {1024000, 16000} row from the vendor driver's
  // coefficient table (pre_div=1, pre_multi=2, adc_div=1, dac_div=1, fs_mode=0,
  // lrck_h=0, lrck_l=0xff, bclk_div=4, adc_osr=0x10, dac_osr=0x10).
  uint8_t reg01 = 0x3F | (1 << 7); // enable all internal clocks; bit7 selects BCLK pin as MCLK source
  ok &= w(ES8311_CLK_MGR_REG01, reg01);

  uint8_t reg02 = r(ES8311_CLK_MGR_REG02) & 0x07;
  reg02 |= (1 - 1) << 5; // pre_div=1
  reg02 |= 2 << 3;       // pre_multi=2 (x4)
  ok &= w(ES8311_CLK_MGR_REG02, reg02);

  ok &= w(ES8311_CLK_MGR_REG03, (0x00 << 6) | 0x10); // fs_mode=single-speed, adc_osr=0x10
  ok &= w(ES8311_CLK_MGR_REG04, 0x10);                // dac_osr=0x10
  ok &= w(ES8311_CLK_MGR_REG05, ((1 - 1) << 4) | (1 - 1)); // adc_div=1, dac_div=1

  uint8_t reg06 = r(ES8311_CLK_MGR_REG06) & 0xE0;
  reg06 |= (4 - 1); // bclk_div=4 (<19, so -1 per the vendor driver's encoding rule)
  ok &= w(ES8311_CLK_MGR_REG06, reg06);

  uint8_t reg07 = r(ES8311_CLK_MGR_REG07) & 0xC0;
  reg07 |= 0x00; // lrck_h
  ok &= w(ES8311_CLK_MGR_REG07, reg07);
  ok &= w(ES8311_CLK_MGR_REG08, 0xFF); // lrck_l

  // Format: slave mode, 32-bit in/out to match the shared bus's slot width.
  uint8_t reg00 = r(ES8311_RESET_REG00) & 0xBF;
  ok &= w(ES8311_RESET_REG00, reg00);
  ok &= w(ES8311_SDPIN_REG09, 4 << 2);  // 32-bit resolution
  ok &= w(ES8311_SDPOUT_REG0A, 4 << 2); // 32-bit resolution

  // Power up analog + DAC path
  ok &= w(ES8311_SYSTEM_REG0D, 0x01); // power up analog circuitry
  ok &= w(ES8311_SYSTEM_REG0E, 0x02); // enable analog PGA, ADC modulator
  ok &= w(ES8311_SYSTEM_REG12, 0x00); // power up DAC
  ok &= w(ES8311_SYSTEM_REG13, 0x10); // enable output drive
  ok &= w(ES8311_ADC_REG1C, 0x6A);    // ADC equalizer bypass, cancel DC offset
  ok &= w(ES8311_DAC_REG37, 0x08);    // bypass DAC equalizer

  // Unmute
  uint8_t reg31 = r(ES8311_DAC_REG31) & (uint8_t)~((1 << 6) | (1 << 5));
  ok &= w(ES8311_DAC_REG31, reg31);

  ok &= ES8311_SetVolume(80);

  return ok;
}

bool ES8311_SetVolume(int volumePercent) {
  if (volumePercent < 0) volumePercent = 0;
  if (volumePercent > 100) volumePercent = 100;
  int reg32 = volumePercent == 0 ? 0 : ((volumePercent * 256 / 100) - 1);
  return w(ES8311_DAC_REG32, (uint8_t)reg32);
}

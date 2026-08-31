#pragma once
#include "TCA9554PWR.h"
#include "Touch_CST816.h"

#define LCD_Backlight_PIN   5
#define PWM_Channel     1       // PWM Channel   
#define Frequency       20000   // PWM frequencyconst        
#define Resolution      10       // PWM resolution ratio     MAX:13
#define Dutyfactor      500     // PWM Dutyfactor      
#define Backlight_MAX   100

#define EXAMPLE_LCD_WIDTH                   (360)
#define EXAMPLE_LCD_HEIGHT                  (360)
#define EXAMPLE_LCD_COLOR_BITS              (16)

#define ESP_PANEL_HOST_SPI_ID_DEFAULT       (SPI2_HOST)
#define ESP_PANEL_LCD_SPI_MODE              (0)                   // 0/1/2/3, typically set to 0
#define ESP_PANEL_LCD_SPI_CLK_HZ            (80 * 1000 * 1000)    // Should be an integer divisor of 80M, typically set to 40M
// Each queued colour transaction can hold an internal-RAM bounce buffer of up
// to ESP_PANEL_HOST_SPI_MAX_TRANSFER_SIZE (spi_master cannot DMA out of PSRAM,
// so the framebuffer is always copied). Depth 10 therefore reserves ~10x2KB of
// the scarcest memory on the board and churns it hard enough to fragment the
// heap that mbedtls needs. 3 is plenty of pipelining for this panel.
#define ESP_PANEL_LCD_SPI_TRANS_QUEUE_SZ    (3)
#define ESP_PANEL_LCD_SPI_CMD_BITS          (32)                  // Typically set to 32
#define ESP_PANEL_LCD_SPI_PARAM_BITS        (8)                   // Typically set to 8

#define ESP_PANEL_LCD_SPI_IO_TE             (18)
#define ESP_PANEL_LCD_SPI_IO_SCK            (40)
#define ESP_PANEL_LCD_SPI_IO_DATA0          (46)
#define ESP_PANEL_LCD_SPI_IO_DATA1          (45)
#define ESP_PANEL_LCD_SPI_IO_DATA2          (42)
#define ESP_PANEL_LCD_SPI_IO_DATA3          (41)
#define ESP_PANEL_LCD_SPI_IO_CS             (21)
#define EXAMPLE_LCD_PIN_NUM_RST             (-1)    // EXIO2
#define EXAMPLE_LCD_PIN_NUM_BK_LIGHT        (-1)

#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL       (1)
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

// esp_lcd splits each colour flush into chunks of this size. Keep it SMALL.
// spi_master will not DMA straight out of PSRAM, so every chunk of the
// framebuffer is copied through a bounce buffer it allocates from internal RAM
// (spi_master.c setup_dma_priv_buffer). With trans_queue_depth = 10 the worst
// case is 10 of these in flight, so this value costs ~10x itself in internal
// DRAM - the scarcest resource on the board, also wanted by WiFi/mbedtls and by
// the I2S DMA descriptors. Raising this to 32768 to cut transaction count made
// the LCD faster on paper and broke both audio ("i2s_std_set_slot: allocate
// memory for dma descriptor failed") and WiFi association.
#define ESP_PANEL_HOST_SPI_MAX_TRANSFER_SIZE   (2048)

extern uint8_t LCD_Backlight;

void ST77916_Init();

void LCD_Init();
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,uint16_t* color);

// backlight
void Backlight_Init();
void Set_Backlight(uint8_t Light);  
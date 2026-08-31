#pragma once

#include <lvgl.h>
#include "lv_conf.h"
// #include <demos/lv_demos.h>  // Commented out for now to fix compilation
#include <esp_heap_caps.h>
#include "Display_ST77916.h"
#include "Touch_CST816.h"

#define LCD_WIDTH     EXAMPLE_LCD_WIDTH
#define LCD_HEIGHT    EXAMPLE_LCD_HEIGHT
// Sized for a whole screen. Under partial refresh (the current setting) this is
// just headroom - LVGL only renders and flushes the dirty area - but it must
// never drop below the full screen while disp_drv.full_refresh is 1, because in
// that mode LVGL treats this as a framebuffer and renders at offsets up to
// LCD_WIDTH*LCD_HEIGHT. It was previously LCD_WIDTH*LCD_HEIGHT/10 *with*
// full_refresh on, so LVGL wrote ~233KB past the end of the allocation and the
// flush DMA'd the overrun to the panel: corrupted image, trashed PSRAM behind
// the buffer, and "panel_io_spi_tx_color: spi transmit (queue) color failed".
#define LVGL_BUF_LEN  (LCD_WIDTH * LCD_HEIGHT)

#define EXAMPLE_LVGL_TICK_PERIOD_MS  10


void Lvgl_print(const char * buf);
void Lvgl_Display_LCD( lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p ); // Displays LVGL content on the LCD.    This function implements associating LVGL data to the LCD screen
void Lvgl_Touchpad_Read( lv_indev_drv_t * indev_drv, lv_indev_data_t * data );                // Read the touchpad
void example_increase_lvgl_tick(void *arg);

void Lvgl_Init(void);
void Lvgl_Loop(void);

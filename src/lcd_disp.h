#pragma once
#include <stdbool.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

bool lcd_disp_init(void);
void lcd_disp_show_startup(void);
void lcd_disp_show_idle(void);
void lcd_disp_show_detected(void);
void lcd_disp_update_mic_level(float level);
void lcd_disp_deinit(void);

#ifdef __cplusplus
}
#endif

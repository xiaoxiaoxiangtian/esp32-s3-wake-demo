#include "lcd_disp.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include <string.h>

static const char *TAG = "lcd_disp";
static esp_lcd_panel_handle_t panel = NULL;
static esp_lcd_panel_io_handle_t io = NULL;
static uint16_t *framebuffer = NULL;
static bool display_ready = false;

#define LCD_W 240
#define LCD_H 240
#define MIC_BAR_X 32
#define MIC_BAR_Y 176
#define MIC_BAR_W 176
#define MIC_BAR_H 18

static uint16_t mic_bar_buffer[MIC_BAR_W * MIC_BAR_H];

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void fill_screen(uint16_t color) {
    if (!display_ready || !framebuffer) return;
    for (int i = 0; i < LCD_W * LCD_H; i++)
        framebuffer[i] = color;
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);
}

static void draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (!framebuffer) return;
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= LCD_H) continue;
        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px >= 0 && px < LCD_W) {
                framebuffer[py * LCD_W + px] = color;
            }
        }
    }
}

// Simple 8x16 font — only chars needed by this demo: A-Z, a-z, 0-9, space, !, .
#define FONT_CHARS 95
static uint8_t font_data[FONT_CHARS * 16] = {0}; // initialized at runtime by init_font()

static void init_font(void) {
    // Space (32): already 0
    // ! (33)
    int i = 1 * 16;
    uint8_t ex[] = {0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0x18,0x18,0,0,0,0};
    memcpy((void*)(font_data + i), ex, 16);
    // . (46)
    i = 14 * 16;
    uint8_t dot[] = {0,0,0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0};
    memcpy((void*)(font_data + i), dot, 16);
    // 0-9 (48-57)
    uint8_t d0[]={0,0,0x3C,0x66,0xC3,0xC3,0xCF,0xDB,0xF3,0xC3,0xC3,0x66,0x3C,0,0,0};
    uint8_t d1[]={0,0,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0};
    uint8_t d2[]={0,0,0x3C,0x66,0xC3,3,6,12,0x18,0x30,0x60,0xC0,0xFF,0,0,0};
    uint8_t d3[]={0,0,0x3C,0x66,0xC3,3,0x1C,0x0E,3,3,0xC3,0x66,0x3C,0,0,0};
    uint8_t d4[]={0,0,6,0x0E,0x1E,0x36,0x66,0xC6,0xFF,6,6,6,6,0,0,0};
    uint8_t d5[]={0,0,0xFF,0xC0,0xC0,0xFE,0xC3,3,3,3,0xC3,0x66,0x3C,0,0,0};
    uint8_t d6[]={0,0,0x3C,0x66,0xC0,0xC0,0xFE,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0,0,0};
    uint8_t d7[]={0,0,0xFF,3,6,6,12,12,0x18,0x18,0x30,0x30,0x60,0,0,0};
    uint8_t d8[]={0,0,0x3C,0x66,0xC3,0x66,0x3C,0x66,0xC3,0xC3,0xC3,0x66,0x3C,0,0,0};
    uint8_t d9[]={0,0,0x3C,0x66,0xC3,0xC3,0xC3,0x7F,3,3,0xC3,0x66,0x3C,0,0,0};
    uint8_t* digits[] = {d0,d1,d2,d3,d4,d5,d6,d7,d8,d9};
    for (int j = 0; j < 10; j++) memcpy((void*)(font_data + (16+j)*16), digits[j], 16);
    // : (58)
    i = 26 * 16;
    uint8_t col[] = {0,0,0,0,0x18,0x18,0,0,0,0x18,0x18,0,0,0,0,0};
    memcpy((void*)(font_data + i), col, 16);
    // A-Z (65-90)
    uint8_t cA[]={0,0,0x18,0x3C,0x66,0xC3,0xC3,0xFF,0xC3,0xC3,0xC3,0xC3,0xC3,0,0,0};
    uint8_t cB[]={0,0,0xFE,0xC3,0xC3,0xC3,0xFE,0xC3,0xC3,0xC3,0xC3,0xC3,0xFE,0,0,0};
    uint8_t cC[]={0,0,0x3C,0x66,0xC3,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0x66,0x3C,0,0,0};
    uint8_t cD[]={0,0,0xFC,0xC6,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC6,0xFC,0,0,0};
    uint8_t cE[]={0,0,0xFF,0xC0,0xC0,0xC0,0xFE,0xC0,0xC0,0xC0,0xC0,0xC0,0xFF,0,0,0};
    uint8_t cF[]={0,0,0xFF,0xC0,0xC0,0xC0,0xFE,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0,0,0};
    uint8_t cG[]={0,0,0x3E,0x63,0xC1,0xC0,0xC0,0xCF,0xC3,0xC3,0xC3,0x67,0x3D,0,0,0};
    uint8_t cH[]={0,0,0xC3,0xC3,0xC3,0xC3,0xFF,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0,0,0};
    uint8_t cI[]={0,0,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0};
    uint8_t cJ[]={0,0,0x3F,12,12,12,12,12,12,12,0xCC,0xCC,0x78,0,0,0};
    uint8_t cK[]={0,0,0xC3,0xC6,0xCC,0xD8,0xF0,0xF0,0xD8,0xCC,0xC6,0xC3,0xC3,0,0,0};
    uint8_t cL[]={0,0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFF,0,0,0};
    uint8_t cM[]={0,0,0xC3,0xE7,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0,0,0};
    uint8_t cN[]={0,0,0xC3,0xE3,0xF3,0xDB,0xCF,0xC7,0xC3,0xC1,0xC0,0xC0,0xC0,0,0,0};
    uint8_t cO[]={0,0,0x3C,0x66,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0,0,0};
    uint8_t cP[]={0,0,0xFE,0xC3,0xC3,0xC3,0xFE,0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0,0,0};
    uint8_t cQ[]={0,0,0x3C,0x66,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0xCF,0x66,0x3D,0,0,0};
    uint8_t cR[]={0,0,0xFE,0xC3,0xC3,0xC3,0xFE,0xD8,0xCC,0xC6,0xC3,0xC3,0xC3,0,0,0};
    uint8_t cS[]={0,0,0x7E,0xC3,0xC0,0xC0,0x7E,3,3,3,0xC3,0xC3,0x7E,0,0,0};
    uint8_t cT[]={0,0,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0};
    uint8_t cU[]={0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0,0,0};
    uint8_t cV[]={0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x3C,0x18,0x18,0,0,0};
    uint8_t cW[]={0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x66,0x66,0,0,0};
    uint8_t cX[]={0,0,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x3C,0x66,0xC3,0xC3,0xC3,0,0,0};
    uint8_t cY[]={0,0,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0};
    uint8_t cZ[]={0,0,0xFF,3,6,12,0x18,0x18,0x30,0x60,0xC0,0xC0,0xFF,0,0,0};
    uint8_t* caps[] = {cA,cB,cC,cD,cE,cF,cG,cH,cI,cJ,cK,cL,cM,cN,cO,cP,cQ,cR,cS,cT,cU,cV,cW,cX,cY,cZ};
    for (int j = 0; j < 26; j++) memcpy((void*)(font_data + (33+j)*16), caps[j], 16);
    // a-z (97-122)
    uint8_t ca[]={0,0,0,0,0x3C,0x66,6,0x3E,0x66,0xC6,0xC6,0x7E,0,0,0,0};
    uint8_t cb[]={0,0xC0,0xC0,0xC0,0xFC,0xC6,0xC3,0xC3,0xC3,0xC3,0xC6,0xFC,0,0,0,0};
    uint8_t cc[]={0,0,0,0,0x3C,0x66,0xC0,0xC0,0xC0,0xC0,0x66,0x3C,0,0,0,0};
    uint8_t cd[]={0,3,3,3,0x3F,0x63,0xC3,0xC3,0xC3,0xC3,0x63,0x3F,0,0,0,0};
    uint8_t ce[]={0,0,0,0,0x3C,0x66,0xC3,0xFF,0xC0,0xC0,0x66,0x3C,0,0,0,0};
    uint8_t cf[]={0,0x1E,0x30,0x30,0xFE,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0,0,0,0};
    uint8_t cg[]={0,0,0,0,0x3F,0x63,0xC3,0xC3,0xC3,0x7F,3,3,0x66,0x3C,0,0};
    uint8_t ch[]={0,0xC0,0xC0,0xC0,0xFC,0xC6,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0,0,0,0};
    uint8_t ci[]={0,0x18,0x18,0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0};
    uint8_t ck[]={0,0xC0,0xC0,0xC0,0xC6,0xCC,0xD8,0xF0,0xF0,0xD8,0xCC,0xC6,0,0,0,0};
    uint8_t cl[]={0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0};
    uint8_t cm[]={0,0,0,0,0x6C,0xFE,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0,0,0,0};
    uint8_t cn[]={0,0,0,0,0xDC,0x66,0x63,0x63,0x63,0x63,0x63,0x63,0,0,0,0};
    uint8_t co[]={0,0,0,0,0x3C,0x66,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0,0,0,0};
    uint8_t cp[]={0,0,0,0,0xDC,0x66,0x63,0x63,0x63,0x7E,0x60,0x60,0x60,0x60,0,0};
    uint8_t cq[]={0,0,0,0,0x3B,0x66,0xC6,0xC6,0xC6,0x7E,6,6,6,6,0,0};
    uint8_t cr[]={0,0,0,0,0xDC,0x66,0x60,0x60,0x60,0x60,0x60,0x60,0,0,0,0};
    uint8_t cs[]={0,0,0,0,0x7E,0xC0,0xC0,0x7E,3,3,0xC3,0x7E,0,0,0,0};
    uint8_t ct[]={0,0x18,0x18,0x18,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x1E,0,0,0,0};
    uint8_t cu[]={0,0,0,0,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3B,0,0,0,0};
    uint8_t cv[]={0,0,0,0,0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x18,0,0,0,0};
    uint8_t cw[]={0,0,0,0,0xC3,0xC3,0xC3,0xDB,0xDB,0xFF,0x66,0x66,0,0,0,0};
    uint8_t cx[]={0,0,0,0,0xC3,0x66,0x3C,0x18,0x3C,0x66,0xC3,0xC3,0,0,0,0};
    uint8_t cy[]={0,0,0,0,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x30,0x30,0x60,0,0,0};
    uint8_t cz[]={0,0,0,0,0xFF,6,12,0x18,0x30,0x60,0xC0,0xFF,0,0,0,0};
    uint8_t* lows[] = {ca,cb,cc,cd,ce,cf,cg,ch,ci,ck,cl,cm,cn,co,cp,cq,cr,cs,ct,cu,cv,cw,cx,cy,cz};
    for (int j = 0; j < 25; j++) memcpy((void*)(font_data + (65+j)*16), lows[j], 16);
}

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg) {
    if (c < 32 || c > 126) c = ' ';
    int base = (c - 32) * 16;
    for (int row = 0; row < 16; row++) {
        uint8_t line = font_data[base + row];
        for (int col = 0; col < 8; col++) {
            int px = x + col, py = y + row;
            if (px >= 0 && px < LCD_W && py >= 0 && py < LCD_H) {
                framebuffer[py * LCD_W + px] = (line & (0x80 >> col)) ? fg : bg;
            }
        }
    }
}

static void draw_string(int x, int y, const char *str, uint16_t fg, uint16_t bg) {
    while (*str) {
        draw_char(x, y, *str++, fg, bg);
        x += 8;
    }
}

static void draw_centered(int y, const char *str, uint16_t fg, uint16_t bg) {
    int len = (int)strlen(str);
    int x = (LCD_W - len * 8) / 2;
    draw_string(x, y, str, fg, bg);
}

bool lcd_disp_init(void)
{
    init_font();
    framebuffer = (uint16_t*)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!framebuffer) {
        framebuffer = (uint16_t*)malloc(LCD_W * LCD_H * 2);
    }
    if (!framebuffer) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer");
        return false;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TFT_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = TFT_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = TFT_CS,
        .dc_gpio_num = TFT_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TFT_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_mirror(panel, false, false);
    esp_lcd_panel_disp_on_off(panel, true);

    gpio_set_direction((gpio_num_t)TFT_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)TFT_BL, 1);

    display_ready = true;
    ESP_LOGI(TAG, "ST7789 initialized: %dx%d", LCD_W, LCD_H);
    return true;
}

void lcd_disp_show_startup(void)
{
    fill_screen(rgb565(0, 0, 0));
    draw_centered(100, "DamiDuo Wake Demo", rgb565(255, 255, 255), rgb565(0, 0, 0));
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);
}

void lcd_disp_show_idle(void)
{
    fill_screen(rgb565(0, 0, 0));
    draw_centered(30, "DamiDuo Wake", rgb565(255, 255, 255), rgb565(0, 0, 0));
    draw_centered(100, "xiao ai tong xue", rgb565(98, 196, 255), rgb565(0, 0, 0));
    draw_centered(150, "MIC", rgb565(120, 130, 140), rgb565(0, 0, 0));
    draw_rect(MIC_BAR_X - 2, MIC_BAR_Y - 2, MIC_BAR_W + 4, MIC_BAR_H + 4, rgb565(55, 60, 68));
    draw_rect(MIC_BAR_X, MIC_BAR_Y, MIC_BAR_W, MIC_BAR_H, rgb565(10, 14, 18));
    draw_centered(208, "Listening...", rgb565(120, 130, 140), rgb565(0, 0, 0));
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);
}

void lcd_disp_show_detected(void)
{
    fill_screen(rgb565(0, 80, 0));
    draw_centered(80, "DETECTED!", rgb565(255, 255, 255), rgb565(0, 80, 0));
    draw_centered(140, "xiao ai tong xue", rgb565(200, 255, 200), rgb565(0, 80, 0));
    esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, framebuffer);
}

void lcd_disp_update_mic_level(float level)
{
    if (!display_ready || !panel) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;

    const int filled = (int)(level * (float)MIC_BAR_W + 0.5f);
    const uint16_t bg = rgb565(10, 14, 18);
    const uint16_t low = rgb565(52, 180, 120);
    const uint16_t mid = rgb565(245, 190, 66);
    const uint16_t high = rgb565(245, 84, 84);

    for (int y = 0; y < MIC_BAR_H; y++) {
        for (int x = 0; x < MIC_BAR_W; x++) {
            uint16_t color = bg;
            if (x < filled) {
                float pos = (float)x / (float)MIC_BAR_W;
                color = pos > 0.78f ? high : (pos > 0.55f ? mid : low);
            }
            mic_bar_buffer[y * MIC_BAR_W + x] = color;
        }
    }

    esp_lcd_panel_draw_bitmap(panel, MIC_BAR_X, MIC_BAR_Y,
                              MIC_BAR_X + MIC_BAR_W, MIC_BAR_Y + MIC_BAR_H,
                              mic_bar_buffer);
}

void lcd_disp_deinit(void)
{
    if (panel) esp_lcd_panel_del(panel);
    if (io) esp_lcd_panel_io_del(io);
    if (framebuffer) free(framebuffer);
    display_ready = false;
    spi_bus_free(SPI2_HOST);
}

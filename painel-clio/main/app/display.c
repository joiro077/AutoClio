// display.c - tela RGB 800x480 + touch GT911 + LVGL (via esp_lvgl_port)
// Pinos conferidos no esquema ESP32-8048S070 V1.1 e no exemplo do fabricante.
#include "display.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "disp";

#define LCD_H 800
#define LCD_V 480
#define PIN_BL 2
#define PIN_TP_SDA 19
#define PIN_TP_SCL 20
#define PIN_TP_RST 38

static lv_display_t *s_disp;

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void display_set_brightness(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_lcd_panel_handle_t panel_init(void)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = CONFIG_PAINEL_LCD_PCLK_HZ,
            .h_res = LCD_H,
            .v_res = LCD_V,
            .hsync_pulse_width = 30,
            .hsync_back_porch = 16,
            .hsync_front_porch = 210,
            .vsync_pulse_width = 13,
            .vsync_back_porch = 10,
            .vsync_front_porch = 22,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = LCD_H * 10,   // evita tremido com framebuffer na PSRAM
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
        .dma_burst_size = 64,
#else
        .psram_trans_align = 64,
#endif
        .hsync_gpio_num = 39,
        .vsync_gpio_num = 40,
        .de_gpio_num = 41,
        .pclk_gpio_num = 42,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            15, 7, 6, 5, 4,        // B0..B4
            9, 46, 3, 8, 16, 1,    // G0..G5
            14, 21, 47, 48, 45,    // R0..R4
        },
        .flags.fb_in_psram = 1,
    };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    return panel;
}

static esp_lcd_touch_handle_t touch_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // a placa ja tem 4k7 externos
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    esp_lcd_touch_config_t tcfg = {
        .x_max = LCD_H,
        .y_max = LCD_V,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = -1,      // INT (IO18 via R17) nao usado: o GT911 e lido por polling
        .flags = {
            .swap_xy = 0,
#ifdef CONFIG_PAINEL_TOUCH_MIRROR_X
            .mirror_x = 1,
#else
            .mirror_x = 0,
#endif
#ifdef CONFIG_PAINEL_TOUCH_MIRROR_Y
            .mirror_y = 1,
#else
            .mirror_y = 0,
#endif
        },
    };
    // O endereco do GT911 (0x5D ou 0x14) depende do nivel do INT no reset.
    // Como o INT nao e controlado, tenta os dois.
    const uint8_t addrs[2] = { ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP };
    // Ate 3 rodadas: o GT911 as vezes nao responde logo depois do reset.
    for (int i = 0; i < 6; i++) {
        if (i && i % 2 == 0) vTaskDelay(pdMS_TO_TICKS(150));
        esp_lcd_panel_io_handle_t io;
        esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        io_cfg.dev_addr = addrs[i % 2];
        io_cfg.scl_speed_hz = 400000;
        if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK) continue;
        esp_lcd_touch_handle_t tp = NULL;
        if (esp_lcd_touch_new_i2c_gt911(io, &tcfg, &tp) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 em 0x%02X", addrs[i % 2]);
            return tp;
        }
        esp_lcd_panel_io_del(io);
    }
    ESP_LOGE(TAG, "touch GT911 nao encontrado");
    return NULL;
}

void display_init(void)
{
    backlight_init();
    esp_lcd_panel_handle_t panel = panel_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;       // LVGL no core 1; OBD/logica no core 0
    port_cfg.task_stack = 8192;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = LCD_H * 60,
        .double_buffer = true,
        .hres = LCD_H,
        .vres = LCD_V,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = { .buff_dma = false, .buff_spiram = true, .swap_bytes = false },
    };
    lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = { .bb_mode = true, .avoid_tearing = false },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);

    esp_lcd_touch_handle_t tp = touch_init();
    if (tp) {
        lvgl_port_touch_cfg_t tcfg = { .disp = s_disp, .handle = tp };
        lvgl_port_add_touch(&tcfg);
    }
    display_set_brightness(90);
}

bool display_lock(void) { return lvgl_port_lock(0); }
void display_unlock(void) { lvgl_port_unlock(); }

/*
 * Каркас застосунку: LVGL 9 + ST7789 240x240 + 3 кнопки.
 *
 * Дисплей: SCLK=9 MOSI=10 CS=21 DC=8 RST=18 BL=13, SPI mode 3, 40 МГц.
 * Кнопки (active-low): ліва=GPIO39 (PREV), середня=GPIO0 (ENTER),
 * права=GPIO40 (NEXT).
 * Підсвітка: LEDC PWM на GPIO13.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "app";

LV_FONT_DECLARE(font_ua_16);
LV_FONT_DECLARE(font_ua_20);

#define PIN_SCLK 9
#define PIN_MOSI 10
#define PIN_CS   14
#define PIN_DC   8
#define PIN_RST  18
#define PIN_BL   13

#define BTN_PREV  39
#define BTN_ENTER 0
#define BTN_NEXT  40

#define LCD_W 240
#define LCD_H 240
#define BUF_LINES 48

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;

/* ---------- Дисплей ---------- */

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    lv_display_flush_ready(s_disp);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, w * h);
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

static void display_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * BUF_LINES * 2 + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 2,
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
}

/* ---------- Підсвітка (PWM) ---------- */

static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));
    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

static void backlight_set(uint8_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------- Кнопки як енкодер: ліва/права = обертання, середня = натиск ---------- */

static void encoder_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool prev_was = false, next_was = false;
    static int64_t hold_start = 0;
    bool prev = !gpio_get_level(BTN_PREV);
    bool next = !gpio_get_level(BTN_NEXT);
    int64_t now = esp_timer_get_time();

    int16_t diff = 0;
    if (next && !next_was) { diff++; hold_start = now; }
    if (prev && !prev_was) { diff--; hold_start = now; }
    prev_was = prev;
    next_was = next;

    /* автоповтор при утриманні — тільки в режимі редагування (слайдер) */
    lv_group_t *grp = lv_indev_get_group(indev);
    if (grp && lv_group_get_editing(grp) && (next || prev)
        && now - hold_start > 400000) {
        diff += next ? 3 : -3;
    }

    data->enc_diff = diff;
    data->state = !gpio_get_level(BTN_ENTER) ? LV_INDEV_STATE_PRESSED
                                             : LV_INDEV_STATE_RELEASED;
}

static void buttons_init(void)
{
    const int btns[] = {BTN_PREV, BTN_ENTER, BTN_NEXT};
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(btns[i]);
        gpio_set_direction(btns[i], GPIO_MODE_INPUT);
        gpio_pullup_en(btns[i]);
    }
}

/* ---------- UI ---------- */

static lv_obj_t *s_counter_label;
static lv_obj_t *s_uptime_label;
static int s_counter = 0;

static void counter_btn_cb(lv_event_t *e)
{
    s_counter++;
    lv_label_set_text_fmt(s_counter_label, "Натиснуто: %d", s_counter);
}

static void slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    backlight_set((uint8_t)lv_slider_get_value(slider));
}

static void uptime_timer_cb(lv_timer_t *t)
{
    lv_label_set_text_fmt(s_uptime_label, "Працює: %lld с",
                          esp_timer_get_time() / 1000000);
}

static void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_set_style_text_font(scr, &font_ua_16, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Плата ESP32-S3");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    /* кнопка-лічильник */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 190, 50);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -35);
    lv_obj_add_event_cb(btn, counter_btn_cb, LV_EVENT_CLICKED, NULL);
    s_counter_label = lv_label_create(btn);
    lv_label_set_text(s_counter_label, "Натиснуто: 0");
    lv_obj_center(s_counter_label);

    /* слайдер яскравості підсвітки */
    lv_obj_t *bright_label = lv_label_create(scr);
    lv_label_set_text(bright_label, "Яскравість");
    lv_obj_set_style_text_color(bright_label, lv_color_hex(0xC0C8D0), 0);
    lv_obj_align(bright_label, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *slider = lv_slider_create(scr);
    lv_obj_set_width(slider, 180);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 55);
    lv_slider_set_range(slider, 10, 255);
    lv_slider_set_value(slider, 255, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_uptime_label = lv_label_create(scr);
    lv_label_set_text(s_uptime_label, "Працює: 0 с");
    lv_obj_set_style_text_color(s_uptime_label, lv_color_hex(0x8090A0), 0);
    lv_obj_align(s_uptime_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_timer_create(uptime_timer_cb, 1000, NULL);

    /* навігація кнопками */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, encoder_read);

    lv_group_t *grp = lv_group_create();
    lv_group_add_obj(grp, btn);
    lv_group_add_obj(grp, slider);
    lv_indev_set_group(indev, grp);
    lv_group_focus_obj(btn);
}

/* ---------- main ---------- */

static uint32_t tick_cb(void) { return esp_timer_get_time() / 1000; }

void app_main(void)
{
    display_init();
    backlight_init();
    buttons_init();

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(s_disp, flush_cb);

    static uint8_t *buf1, *buf2;
    size_t buf_sz = LCD_W * BUF_LINES * 2;
    buf1 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    buf2 = heap_caps_malloc(buf_sz, MALLOC_CAP_DMA);
    assert(buf1 && buf2);
    lv_display_set_buffers(s_disp, buf1, buf2, buf_sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ui_init();
    ESP_LOGI(TAG, "LVGL запущено. Ліва/права — фокус, середня — дія.");

    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 100) delay_ms = 100;
        if (delay_ms < 5) delay_ms = 5;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

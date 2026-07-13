/*
 * Інтернет-радіо для XingZhi Cube 1.54TFT (ESP32-S3).
 *
 * Керування:
 *   ліва/права — попередня/наступна станція (одразу грає)
 *   середня коротко — відтворення/стоп
 *   середня довго (>0.6 с) — режим гучності: ліва/права = тихіше/гучніше
 *     (вихід — ще одне довге або 5 с бездіяльності)
 *
 * RGB LED: жовтий = з'єднання/буферизація, зелений = грає,
 *          червоний = помилка, вимкнений = стоп.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "led_strip.h"
#include "lvgl.h"
#include "audio.h"
#include "wifi.h"
#include "wifi_creds.h"

static const char *TAG = "radio";

LV_FONT_DECLARE(font_ua_16);
LV_FONT_DECLARE(font_ua_20);

/* --- залізо --- */
#define PIN_SCLK 9
#define PIN_MOSI 10
#define PIN_CS   14
#define PIN_DC   8
#define PIN_RST  18
#define PIN_BL   13
#define BTN_LEFT  39
#define BTN_MID   0
#define BTN_RIGHT 40
#define RGB_PIN  48
#define LCD_W 240
#define LCD_H 240
#define BUF_LINES 48

/* --- станції --- */
typedef struct { const char *name; const char *url; } station_t;
static const station_t STATIONS[] = {
    { "SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3" },
    { "SomaFM Secret Agent", "http://ice1.somafm.com/secretagent-128-mp3" },
    { "Radio Paradise",      "http://stream.radioparadise.com/mp3-128" },
    { "Хіт FM",              "https://online.hitfm.ua/HitFM" },
    { "Радіо ROKS",          "https://online.radioroks.ua/RadioROKS" },
};
#define N_STATIONS (sizeof(STATIONS) / sizeof(STATIONS[0]))

static int s_station = 0;
static bool s_want_play = true;
static bool s_vol_mode = false;

/* --- дисплей --- */
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static led_strip_handle_t s_led;

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

    /* підсвітка на повну через LEDC (можна буде додати регулювання) */
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

static void led_init(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = RGB_PIN,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = { .resolution_hz = 10 * 1000 * 1000 };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led));
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

/* --- UI --- */
static lv_obj_t *s_lbl_station;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_info;
static lv_obj_t *s_bar_vol;
static lv_obj_t *s_lbl_vol;

static void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_text_font(scr, &font_ua_16, 0);

    lv_obj_t *hdr = lv_label_create(scr);
    lv_label_set_text(hdr, "Інтернет-радіо");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x5A6672), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 8);

    s_lbl_station = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_station, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_lbl_station, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(s_lbl_station, 220);
    lv_obj_set_style_text_align(s_lbl_station, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_station, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_lbl_station, LV_ALIGN_CENTER, 0, -45);

    s_lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xC0C8D0), 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, -10);

    s_lbl_info = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_info, lv_color_hex(0x5A6672), 0);
    lv_obj_align(s_lbl_info, LV_ALIGN_CENTER, 0, 18);

    s_bar_vol = lv_bar_create(scr);
    lv_obj_set_size(s_bar_vol, 180, 10);
    lv_obj_align(s_bar_vol, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_bar_set_range(s_bar_vol, 0, 256);
    lv_bar_set_value(s_bar_vol, audio_get_volume(), LV_ANIM_OFF);

    s_lbl_vol = lv_label_create(scr);
    lv_obj_set_style_text_color(s_lbl_vol, lv_color_hex(0x5A6672), 0);
    lv_obj_align(s_lbl_vol, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(s_lbl_vol, "");
}

static void ui_apply_station(void)
{
    lv_label_set_text(s_lbl_station, STATIONS[s_station].name);
}

/* --- кнопки (опитування з LVGL-таймера) --- */

static void start_playback(void)
{
    s_want_play = true;
    audio_play(STATIONS[s_station].url);
}

static void buttons_poll_cb(lv_timer_t *t)
{
    static bool l_was, m_was, r_was;
    static int64_t m_down_at;
    static bool m_long_fired;
    static int64_t last_activity;
    static int64_t l_down_at, r_down_at, last_repeat;

    int64_t now = esp_timer_get_time();
    bool l = !gpio_get_level(BTN_LEFT);
    bool m = !gpio_get_level(BTN_MID);
    bool r = !gpio_get_level(BTN_RIGHT);

    /* середня: довге натискання = режим гучності, коротке = play/stop */
    if (m && !m_was) { m_down_at = now; m_long_fired = false; }
    if (m && !m_long_fired && now - m_down_at > 600000) {
        m_long_fired = true;
        s_vol_mode = !s_vol_mode;
        last_activity = now;
    }
    if (!m && m_was && !m_long_fired) {
        if (s_vol_mode) {
            s_vol_mode = false;
        } else if (audio_state() == AUDIO_STOPPED ||
                   audio_state() == AUDIO_ERROR) {
            start_playback();
        } else {
            s_want_play = false;
            audio_stop();
        }
        last_activity = now;
    }

    /* ліва/права */
    bool l_edge = l && !l_was, r_edge = r && !r_was;
    if (l_edge) l_down_at = now;
    if (r_edge) r_down_at = now;

    if (s_vol_mode) {
        int step = 0;
        if (l_edge) step = -16;
        if (r_edge) step = +16;
        /* автоповтор при утриманні */
        if (!step && (l || r) && now - (l ? l_down_at : r_down_at) > 400000
            && now - last_repeat > 120000) {
            step = l ? -8 : +8;
            last_repeat = now;
        }
        if (step) {
            audio_set_volume(audio_get_volume() + step);
            last_activity = now;
        }
        /* автовихід */
        if (now - last_activity > 5000000) s_vol_mode = false;
    } else {
        if (l_edge || r_edge) {
            s_station = (s_station + (r_edge ? 1 : N_STATIONS - 1))
                        % N_STATIONS;
            ui_apply_station();
            start_playback();
        }
    }

    l_was = l; m_was = m; r_was = r;
}

/* --- періодичне оновлення статусу --- */

static void status_timer_cb(lv_timer_t *t)
{
    audio_state_t st = audio_state();
    switch (st) {
    case AUDIO_STOPPED:
        lv_label_set_text(s_lbl_status, "Зупинено");
        led_set(0, 0, 0);
        break;
    case AUDIO_CONNECTING:
        lv_label_set_text(s_lbl_status, "З'єднання...");
        led_set(20, 12, 0);
        break;
    case AUDIO_BUFFERING:
        lv_label_set_text(s_lbl_status, "Буферизація...");
        led_set(20, 12, 0);
        break;
    case AUDIO_PLAYING:
        lv_label_set_text(s_lbl_status, "Грає");
        led_set(0, 18, 0);
        break;
    case AUDIO_ERROR:
        lv_label_set_text(s_lbl_status, "Помилка потоку");
        led_set(24, 0, 0);
        break;
    }
    lv_label_set_text(s_lbl_info, audio_info());

    lv_bar_set_value(s_bar_vol, audio_get_volume(), LV_ANIM_OFF);
    if (s_vol_mode) {
        lv_label_set_text_fmt(s_lbl_vol, "Гучність: %d%%",
                              audio_get_volume() * 100 / 256);
        lv_obj_set_style_bg_color(s_bar_vol, lv_color_hex(0x3B82F6),
                                  LV_PART_INDICATOR);
    } else {
        lv_label_set_text_fmt(s_lbl_vol, "%d%%",
                              audio_get_volume() * 100 / 256);
        lv_obj_set_style_bg_color(s_bar_vol, lv_color_hex(0x4A5560),
                                  LV_PART_INDICATOR);
    }
}

/* --- main --- */

static uint32_t tick_cb(void) { return esp_timer_get_time() / 1000; }

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    display_init();
    led_init();

    const int btns[] = {BTN_LEFT, BTN_MID, BTN_RIGHT};
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(btns[i]);
        gpio_set_direction(btns[i], GPIO_MODE_INPUT);
        gpio_pullup_en(btns[i]);
    }

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
    ui_apply_station();
    lv_label_set_text(s_lbl_status, "Wi-Fi...");
    led_set(20, 12, 0);
    lv_refr_now(s_disp);

    bool wifi_ok = wifi_connect_blocking(WIFI_SSID, WIFI_PASS, 20000);

    audio_init();

    if (wifi_ok) {
        ESP_LOGI(TAG, "Wi-Fi OK, стартуємо станцію: %s",
                 STATIONS[s_station].name);
        start_playback();
    } else {
        lv_label_set_text(s_lbl_status, "Wi-Fi не під'єднано!");
        led_set(24, 0, 0);
    }

    lv_timer_create(buttons_poll_cb, 30, NULL);
    lv_timer_create(status_timer_cb, 300, NULL);

    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 50) delay_ms = 50;
        if (delay_ms < 5) delay_ms = 5;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

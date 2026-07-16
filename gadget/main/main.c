/*
 * Робочий стіл робота: головний екран з анімованим обличчям (очі + рот),
 * меню й застосунки (Wi-Fi, погода, радіо, новини).
 *
 * Дисплей: SCLK=9 MOSI=10 CS=14 DC=8 RST=18 BL=13, SPI mode 3, 40 МГц.
 * Кнопки: ліва=39, середня=0, права=40 (active-low).
 * Керування: ліва/права — навігація, середня — вибір,
 *            середня довго (>0.6 c) — назад.
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
#include "esp_random.h"
#include "esp_log.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
#include "settings.h"
#include "battery.h"
#include "led.h"
#include "audio.h"

static const char *TAG = "desk";

#define PIN_SCLK 9
#define PIN_MOSI 10
#define PIN_CS   14
#define PIN_DC   8
#define PIN_RST  18
#define PIN_BL   13
#define BTN_LEFT  39
#define BTN_MID   0
#define BTN_RIGHT 40
#define LCD_W 240
#define LCD_H 240
#define BUF_LINES 40
#define COL_FACE 0x35C4F0

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;

/* ---------------- дисплей ---------------- */

static bool on_trans_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    lv_display_flush_ready(s_disp);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px, w * h);
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px);
}

static void display_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * BUF_LINES * 2 + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 3, .trans_queue_depth = 2,
        .on_color_trans_done = on_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io));
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = PIN_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);
    ledc_channel_config_t cc = {
        .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 255,
    };
    ledc_channel_config(&cc);
}

void ui_backlight_set(uint8_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ---------------- екрани ---------------- */

typedef enum { SCR_HOME, SCR_MENU, SCR_APP } screen_t;
static screen_t s_screen = SCR_HOME;
static const app_t *s_app = NULL;

static const app_t *const APPS[] = { &app_gemini, &app_weather, &app_radio,
                                     &app_news, &app_ble, &app_wifitools,
                                     &app_modules, &app_light, &app_settings };
#define GEMINI_IDX 0
#define N_APPS (int)(sizeof(APPS) / sizeof(APPS[0]))
static int s_menu_sel = 0;

/* ---------------- головний екран: очі + рот ---------------- */

static lv_obj_t *s_eye_l, *s_eye_r, *s_mouth;
static lv_obj_t *s_clock_lbl;
static lv_timer_t *s_face_timer, *s_clock_timer, *s_drift_timer;

/* стан «сну» (під час притлумлення підсвітки на головному екрані) */
static lv_obj_t *s_bub[3];
static bool s_sleeping;

static void clock_tick(lv_timer_t *t)
{
    char clk[8];
    netcfg_get_clock(clk, sizeof(clk));
    if (s_clock_lbl) lv_label_set_text(s_clock_lbl, clk);
}
#define EYE_W 50
#define EYE_H 62
#define EYE_CX 40
#define EYE_CY 96
#define MOUTH_CY 168

/* динамічні параметри обличчя */
static float g_bl, g_br;      /* кліпання лівого/правого ока 0..1 */
static float g_lx, g_ly;      /* напрям погляду (зсув очей) */
static float g_ox, g_oy;      /* дрейф усього обличчя (проти вигоряння) */
static float g_mw = 70, g_mh = 16; /* розмір рота */

static void face_render(void)
{
    int cx = 120 + (int)g_ox;
    int ey = EYE_CY + (int)g_oy + (int)g_ly;
    int lx = (int)g_lx;
    int hl = (int)(EYE_H * (1 - g_bl)); if (hl < 4) hl = 4;
    int hr = (int)(EYE_H * (1 - g_br)); if (hr < 4) hr = 4;
    lv_obj_set_size(s_eye_l, EYE_W, hl);
    lv_obj_set_pos(s_eye_l, cx - EYE_CX - EYE_W / 2 + lx, ey - hl / 2);
    lv_obj_set_size(s_eye_r, EYE_W, hr);
    lv_obj_set_pos(s_eye_r, cx + EYE_CX - EYE_W / 2 + lx, ey - hr / 2);
    int mw = (int)g_mw, mh = (int)g_mh;
    lv_obj_set_size(s_mouth, mw, mh);
    lv_obj_set_pos(s_mouth, cx - mw / 2 + lx / 2, MOUTH_CY + (int)g_oy - mh / 2);
}

/* exec-колбеки: кожен — на «свій» глобал (унікальний var для паралельних анімацій) */
static void e_bb(void *v, int32_t a) { g_bl = g_br = a / 1000.0f; face_render(); }
static void e_bl(void *v, int32_t a) { g_bl = a / 1000.0f; face_render(); }
static void e_br(void *v, int32_t a) { g_br = a / 1000.0f; face_render(); }
static void e_lx(void *v, int32_t a) { g_lx = a; face_render(); }
static void e_ly(void *v, int32_t a) { g_ly = a; face_render(); }
static void e_ox(void *v, int32_t a) { g_ox = a; face_render(); }
static void e_oy(void *v, int32_t a) { g_oy = a; face_render(); }
static void e_mw(void *v, int32_t a) { g_mw = a; g_mh = 10 + (a - 40) / 4; face_render(); }

static void anim1(void *var, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                  uint32_t t, uint32_t pb, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, t);
    if (pb) lv_anim_set_playback_duration(&a, pb);
    if (path) lv_anim_set_path_cb(&a, path);
    lv_anim_start(&a);
}

static void face_tick(lv_timer_t *t)
{
    uint32_t r = esp_random() % 100;
    if (r < 28) {                         /* кліпання обома */
        anim1(&g_bl, e_bb, 0, 1000, 70, 110, NULL);
    } else if (r < 36) {                  /* підморгування одним */
        if (esp_random() & 1) anim1(&g_bl, e_bl, 0, 1000, 80, 130, NULL);
        else anim1(&g_br, e_br, 0, 1000, 80, 130, NULL);
    } else if (r < 56) {                  /* глянути кудись */
        int tx = (int)(esp_random() % 37) - 18;
        int ty = (int)(esp_random() % 21) - 10;
        anim1(&g_lx, e_lx, (int)g_lx, tx, 260, 0, lv_anim_path_ease_out);
        anim1(&g_ly, e_ly, (int)g_ly, ty, 260, 0, lv_anim_path_ease_out);
    } else if (r < 70) {                  /* повернути погляд у центр */
        anim1(&g_lx, e_lx, (int)g_lx, 0, 300, 0, lv_anim_path_ease_out);
        anim1(&g_ly, e_ly, (int)g_ly, 0, 300, 0, lv_anim_path_ease_out);
    } else if (r < 85) {                  /* «розмова» ротом */
        anim1(&g_mw, e_mw, 45, 60 + esp_random() % 36, 150, 190, NULL);
    } else {                              /* примружитись */
        anim1(&g_bl, e_bb, 0, 500, 200, 450, lv_anim_path_ease_in_out);
    }
    lv_timer_set_period(t, 500 + esp_random() % 1500);
}

/* повільний дрейф усього обличчя по екрану — проти вигоряння пікселів */
static void drift_tick(lv_timer_t *t)
{
    int tx = (int)(esp_random() % 45) - 22;
    int ty = (int)(esp_random() % 29) - 14;
    anim1(&g_ox, e_ox, (int)g_ox, tx, 2500, 0, lv_anim_path_ease_in_out);
    anim1(&g_oy, e_oy, (int)g_oy, ty, 2500, 0, lv_anim_path_ease_in_out);
    lv_timer_set_period(t, 9000 + esp_random() % 9000);
}

static lv_obj_t *make_face_el(lv_obj_t *p, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_style_bg_color(o, lv_color_hex(COL_FACE), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    return o;
}

static void show_home(void)
{
    s_screen = SCR_HOME;
    netcfg_wifi_stop();          /* енергоощадність: на екрані очікування Wi-Fi off */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x06080C), 0);
    lv_obj_set_style_text_font(scr, &font_ua_16, 0);

    s_eye_l = make_face_el(scr, 18);
    s_eye_r = make_face_el(scr, 18);
    s_mouth = make_face_el(scr, 12);
    /* скинути динамічні параметри */
    g_bl = g_br = g_lx = g_ly = g_ox = g_oy = 0;
    g_mw = 70; g_mh = 16;
    face_render();

    /* годинник (24г) угорі */
    char clk[8];
    netcfg_get_clock(clk, sizeof(clk));
    s_clock_lbl = lv_label_create(scr);
    lv_label_set_text(s_clock_lbl, clk);
    lv_obj_set_style_text_font(s_clock_lbl, &font_num_34, 0);
    lv_obj_set_style_text_color(s_clock_lbl, lv_color_hex(0x5A6672), 0);
    lv_obj_align(s_clock_lbl, LV_ALIGN_TOP_MID, 0, 6);

    s_face_timer = lv_timer_create(face_tick, 1400, NULL);
    s_clock_timer = lv_timer_create(clock_tick, 5000, NULL);
    s_drift_timer = lv_timer_create(drift_tick, 6000, NULL);

    s_sleeping = false;
    for (int i = 0; i < 3; i++) s_bub[i] = NULL;
}

/* ---- анімація сну (очі заплющені + бульбашки, що піднімаються й тануть) ---- */
static void e_bub_y(void *v, int32_t y)  { lv_obj_set_y((lv_obj_t *)v, y); }
static void e_bub_op(void *v, int32_t o) { lv_obj_set_style_bg_opa((lv_obj_t *)v, o, 0); }

static void face_bubbles_stop(void)
{
    for (int i = 0; i < 3; i++)
        if (s_bub[i]) { lv_anim_delete(s_bub[i], NULL); lv_obj_delete(s_bub[i]); s_bub[i] = NULL; }
}

/* заснути: спокійне заплющене обличчя + бульбашки (виклик при притлумленні) */
static void face_sleep(void)
{
    if (s_screen != SCR_HOME || s_sleeping) return;
    s_sleeping = true;
    if (s_face_timer) lv_timer_pause(s_face_timer);
    if (s_drift_timer) lv_timer_pause(s_drift_timer);
    lv_anim_delete_all();                 /* зупинити випадкові анімації обличчя */
    g_bl = g_br = 1.0f; g_lx = g_ly = g_ox = g_oy = 0;   /* очі заплющені, погляд рівний */
    g_mw = 44; g_mh = 8;                  /* маленький спокійний рот */
    face_render();

    lv_obj_t *scr = lv_screen_active();
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_obj_create(scr);
        int d = 7 + i * 3;
        lv_obj_set_size(b, d, d);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x5A6672), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(b, 150 + i * 11, 88);
        s_bub[i] = b;
        /* нескінченний повтор зі зсувом фази для кожної бульбашки */
        lv_anim_t ay; lv_anim_init(&ay); lv_anim_set_var(&ay, b);
        lv_anim_set_exec_cb(&ay, e_bub_y); lv_anim_set_values(&ay, 92, 44);
        lv_anim_set_duration(&ay, 2600); lv_anim_set_delay(&ay, i * 850);
        lv_anim_set_repeat_count(&ay, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_in_out); lv_anim_start(&ay);
        lv_anim_t ao; lv_anim_init(&ao); lv_anim_set_var(&ao, b);
        lv_anim_set_exec_cb(&ao, e_bub_op); lv_anim_set_values(&ao, 200, 0);
        lv_anim_set_duration(&ao, 2600); lv_anim_set_delay(&ao, i * 850);
        lv_anim_set_repeat_count(&ao, LV_ANIM_REPEAT_INFINITE); lv_anim_start(&ao);
    }
}

/* прокинутись: очі розплющені, бульбашки прибрані, анімації відновлені */
static void face_wake(void)
{
    if (!s_sleeping) return;
    s_sleeping = false;
    face_bubbles_stop();
    g_bl = g_br = 0; g_mw = 70; g_mh = 16;
    face_render();
    if (s_face_timer) lv_timer_resume(s_face_timer);
    if (s_drift_timer) lv_timer_resume(s_drift_timer);
}

/* екран вимикається: прибрати бульбашки, щоб не гнати рендери в темряву */
static void face_off(void)
{
    face_bubbles_stop();      /* очі лишаються заплющені статично — жодних анімацій */
}

static void home_leave(void)
{
    face_bubbles_stop();       /* прибрати бульбашки до очищення екрана (без dangling) */
    s_sleeping = false;
    if (s_face_timer) { lv_timer_delete(s_face_timer); s_face_timer = NULL; }
    if (s_clock_timer) { lv_timer_delete(s_clock_timer); s_clock_timer = NULL; }
    if (s_drift_timer) { lv_timer_delete(s_drift_timer); s_drift_timer = NULL; }
    lv_anim_delete_all();
}

/* ---------------- меню (карусель великих іконок) ---------------- */

#define CARD_W 240
static lv_obj_t *s_track, *s_menu_wifi, *s_menu_bat, *s_menu_clock;
static lv_timer_t *s_menu_timer;

static void menu_wifi_tick(lv_timer_t *t)
{
    if (s_menu_wifi)
        lv_obj_set_style_text_color(s_menu_wifi,
            lv_color_hex(netcfg_is_connected() ? 0xFFFFFF : 0x3A4550), 0);
    if (s_menu_clock) {
        char clk[8];
        netcfg_get_clock(clk, sizeof(clk));
        lv_label_set_text(s_menu_clock, clk);
    }
    /* батарея (ADC2 читаємо рідше, бо ділиться з Wi-Fi) */
    static int cnt = 0;
    if (s_menu_bat && (cnt++ % 4) == 0) {
        battery_sample();
        int lvl = battery_level();
        if (lvl < 0) lv_label_set_text(s_menu_bat, "--%");
        else lv_label_set_text_fmt(s_menu_bat, "%d%%", lvl);
        uint32_t col = battery_charging() ? 0x35C4F0 :
                       lvl < 0  ? 0x5A6672 :
                       lvl > 50 ? 0x4ADE80 :
                       lvl > 20 ? 0xFACC15 : 0xF87171;
        lv_obj_set_style_text_color(s_menu_bat, lv_color_hex(col), 0);
    }
}

static void menu_leave(void)
{
    if (s_menu_timer) { lv_timer_delete(s_menu_timer); s_menu_timer = NULL; }
}

/* --- примітиви іконок --- */
#define IC_ACC 0x35C4F0
#define IC_CL  0xC7CED6
#define IC_YE  0xFFD43B
#define IC_GY  0x8A94A0
static void idot(lv_obj_t *p, int d, uint32_t c, int x, int y)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, d, d); lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}
static void irect(lv_obj_t *p, int w, int h, int r, uint32_t c, int x, int y)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, w, h); lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}

/* контейнер 64x64 з іконкою застосунку за індексом */
static lv_obj_t *make_app_icon(lv_obj_t *parent, int idx)
{
    lv_obj_t *ic = lv_obj_create(parent);
    lv_obj_set_size(ic, 64, 64);
    lv_obj_set_style_bg_opa(ic, LV_OPA_0, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_pad_all(ic, 0, 0);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ic, LV_SCROLLBAR_MODE_OFF);
    switch (idx) {
    case 0: /* Асистент — мікрофон */
        irect(ic, 18, 28, 9, IC_ACC, 23, 8);
        irect(ic, 26, 4, 2, IC_ACC, 19, 40);
        irect(ic, 4, 10, 2, IC_ACC, 30, 40);
        irect(ic, 20, 4, 2, IC_ACC, 22, 50);
        break;
    case 1: /* Погода — сонце + хмара */
        idot(ic, 20, IC_YE, 8, 8);
        idot(ic, 15, IC_CL, 18, 28); idot(ic, 20, IC_CL, 28, 22);
        idot(ic, 15, IC_CL, 40, 28); irect(ic, 38, 11, 5, IC_CL, 18, 33);
        break;
    case 2: /* Радіо — нота */
        idot(ic, 15, IC_ACC, 16, 40);
        irect(ic, 4, 32, 2, IC_ACC, 29, 12);
        irect(ic, 11, 5, 2, IC_ACC, 31, 12);
        break;
    case 3: /* Новини — газета */
        irect(ic, 44, 42, 4, IC_CL, 10, 11);
        irect(ic, 30, 7, 1, IC_ACC, 17, 17);
        irect(ic, 30, 3, 1, IC_GY, 17, 30);
        irect(ic, 30, 3, 1, IC_GY, 17, 37);
        irect(ic, 20, 3, 1, IC_GY, 17, 44);
        break;
    case 4: /* BLE — сигнальні смуги */
        irect(ic, 7, 10, 2, IC_ACC, 15, 38);
        irect(ic, 7, 18, 2, IC_ACC, 27, 30);
        irect(ic, 7, 26, 2, IC_ACC, 39, 22);
        break;
    case 5: /* Wi-Fi атака — радіохвилі */
        idot(ic, 8, IC_ACC, 28, 44);
        irect(ic, 18, 5, 2, IC_ACC, 23, 33);
        irect(ic, 30, 5, 2, IC_ACC, 17, 23);
        irect(ic, 42, 5, 2, IC_ACC, 11, 13);
        break;
    case 6: /* Модулі — чіп */
        irect(ic, 34, 34, 4, IC_ACC, 15, 15);
        irect(ic, 14, 14, 2, 0x0B0F14, 25, 25);
        irect(ic, 4, 4, 0, IC_ACC, 9, 22); irect(ic, 4, 4, 0, IC_ACC, 9, 38);
        irect(ic, 4, 4, 0, IC_ACC, 51, 22); irect(ic, 4, 4, 0, IC_ACC, 51, 38);
        break;
    case 7: /* Нічник — лампа */
        idot(ic, 26, IC_YE, 19, 6);
        irect(ic, 14, 8, 2, IC_GY, 25, 32);
        irect(ic, 10, 4, 1, IC_GY, 27, 40);
        break;
    default: /* Налаштування — повзунки */
        for (int k = 0; k < 3; k++) {
            irect(ic, 40, 4, 2, IC_GY, 12, 16 + k * 13);
            idot(ic, 10, IC_ACC, k == 0 ? 16 : k == 1 ? 38 : 27, 13 + k * 13);
        }
        break;
    }
    return ic;
}

/* --- анімація свайпу --- */
static void track_exec(void *v, int32_t x) { lv_obj_set_x(s_track, x); }

static void menu_slide(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_track);
    lv_anim_set_exec_cb(&a, track_exec);
    lv_anim_set_values(&a, lv_obj_get_x(s_track), -s_menu_sel * CARD_W);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void show_menu(void)
{
    s_screen = SCR_MENU;
    netcfg_wifi_resume();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_text_font(scr, &font_ua_16, 0);

    /* хедер: годинник по центру, Wi-Fi зліва, батарея справа */
    s_menu_clock = lv_label_create(scr);
    lv_label_set_text(s_menu_clock, "--:--");
    lv_obj_set_style_text_font(s_menu_clock, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_menu_clock, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_menu_clock, LV_ALIGN_TOP_MID, 0, 8);

    s_menu_wifi = lv_label_create(scr);
    lv_label_set_text(s_menu_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_menu_wifi, &lv_font_montserrat_14, 0);
    lv_obj_align(s_menu_wifi, LV_ALIGN_TOP_LEFT, 6, 12);

    s_menu_bat = lv_label_create(scr);
    lv_label_set_text(s_menu_bat, "--%");
    lv_obj_set_style_text_color(s_menu_bat, lv_color_hex(0x5A6672), 0);
    lv_obj_align(s_menu_bat, LV_ALIGN_TOP_RIGHT, -6, 12);

    menu_wifi_tick(NULL);
    s_menu_timer = lv_timer_create(menu_wifi_tick, 1000, NULL);

    /* доріжка карусельних карток */
    s_track = lv_obj_create(scr);
    lv_obj_set_size(s_track, CARD_W * N_APPS, 190);
    lv_obj_set_pos(s_track, -s_menu_sel * CARD_W, 40);
    lv_obj_set_style_bg_opa(s_track, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_track, 0, 0);
    lv_obj_set_style_pad_all(s_track, 0, 0);
    lv_obj_clear_flag(s_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_track, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < N_APPS; i++) {
        lv_obj_t *card = lv_obj_create(s_track);
        lv_obj_set_size(card, CARD_W, 190);
        lv_obj_set_pos(card, i * CARD_W, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *ic = make_app_icon(card, i);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 24);

        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, APPS[i]->name);
        lv_obj_set_style_text_font(l, &font_ua_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8ECF0), 0);
        lv_obj_set_width(l, 200);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 104);
    }

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "<  центр — відкрити  >");   /* наш кириличний шрифт */
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
}

/* ---------------- перемикання ---------------- */

static void open_app(int idx)
{
    s_screen = SCR_APP;
    s_app = APPS[idx];
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_text_font(scr, &font_ua_16, 0);
    s_app->open(scr);
}

static void close_app(void)
{
    if (s_app) { s_app->close(); s_app = NULL; }
}

/* ---------------- кнопки ---------------- */

/* ---- автозгасання підсвітки на простої ---- */
#define BL_DIM_US 15000000LL   /* 15 с бездіяльності → притлумити */
#define BL_OFF_US 60000000LL   /* 60 с → погасити екран */
static int64_t s_last_act;     /* час останнього натискання */
static uint8_t s_bl_state;     /* 0=повна, 1=тьмяна, 2=вимкнена */
static bool s_bl_swallow;      /* «ковтнути» клік, що розбудив з вимкненого екрана */

/* натискання будь-якої кнопки: скинути таймер простою й відновити яскравість */
static void bl_wake(void)
{
    s_last_act = esp_timer_get_time();
    if (s_bl_state != 0) {
        if (s_bl_state == 2) s_bl_swallow = true;  /* пробудження з OFF — цей клік не діє */
        ui_backlight_set(settings_brightness());
        s_bl_state = 0;
        face_wake();                                /* робот прокидається */
    }
}

static void buttons_poll_cb(lv_timer_t *t)
{
    static bool was[3];
    static int64_t down_at[3], last_rep[3];
    static bool mid_long;
    static bool side_long[3];   /* утримання бокової кнопки вже оброблене */
    /* антидребезг: приймаємо зміну рівня лише коли він стабільний ≥2 полінги
       (~50 мс при періоді 25 мс) — інакше механічний брязкіт дає хибні кліки */
    static bool deb[3], rawprev[3];
    static uint8_t stab[3];
    const int pins[3] = { BTN_LEFT, BTN_MID, BTN_RIGHT };
    int64_t now = esp_timer_get_time();

    /* керування яскравістю за простоєм */
    if (s_last_act == 0) s_last_act = now;
    int64_t idle = now - s_last_act;
    if (s_bl_state == 0 && idle > BL_DIM_US) {
        int d = settings_brightness() / 4; if (d < 8) d = 8;
        ui_backlight_set(d); s_bl_state = 1;         /* притлумити */
        if (s_screen == SCR_HOME) face_sleep();      /* робот засинає */
    } else if (s_bl_state == 1 && idle > BL_OFF_US) {
        ui_backlight_set(0); s_bl_state = 2;         /* погасити */
        if (s_screen == SCR_HOME) face_off();
    }

    bool all_released = true;
    for (int i = 0; i < 3; i++) {
        bool raw = !gpio_get_level(pins[i]);
        if (raw) all_released = false;
        if (raw != rawprev[i]) { rawprev[i] = raw; stab[i] = 0; }
        else if (stab[i] < 255) stab[i]++;
        if (stab[i] >= 2) deb[i] = raw;   /* стабільний рівень — фіксуємо */
        bool pressed = deb[i];
        bool clicked = false;

        if (pressed && !was[i]) {
            down_at[i] = now;
            if (i == BTN_MID_ID) mid_long = false;
            else side_long[i] = false;
            bl_wake();          /* натискання будить екран і скидає простій */
        }
        if (i == BTN_MID_ID) {
            /* на головному екрані утримання 5 с = виклик Gemini */
            if (pressed && !mid_long && s_screen == SCR_HOME &&
                now - down_at[i] > 5000000) {
                mid_long = true;
                home_leave();
                open_app(GEMINI_IDX);
                was[i] = pressed;
                continue;
            }
            if (pressed && !mid_long && s_screen != SCR_HOME &&
                now - down_at[i] > 600000) {
                mid_long = true;
                if (s_screen == SCR_APP) { close_app(); show_menu(); }
                else if (s_screen == SCR_MENU) { menu_leave(); show_home(); }
                was[i] = pressed;
                continue;
            }
            if (!pressed && was[i] && !mid_long) clicked = true;
        } else if (s_screen == SCR_APP && s_app && s_app->on_hold) {
            /* застосунок хоче окрему подію утримання бокової кнопки:
               короткий тап — по відпусканню; утримання ~700 мс — on_hold */
            if (pressed && !side_long[i] && now - down_at[i] > 700000) {
                side_long[i] = true;
                s_app->on_hold(i);
                was[i] = pressed;
                continue;
            }
            if (!pressed && was[i] && !side_long[i]) clicked = true;
        } else {
            if (pressed && !was[i]) clicked = true;
            if (pressed && now - down_at[i] > 400000 &&
                now - last_rep[i] > 150000) { clicked = true; last_rep[i] = now; }
        }
        was[i] = pressed;
        if (!clicked) continue;
        if (s_bl_swallow) continue;   /* клік лише розбудив екран — не діємо */

        switch (s_screen) {
        case SCR_HOME:
            home_leave();
            show_menu();
            break;
        case SCR_MENU:
            if (i == BTN_LEFT_ID) { if (s_menu_sel > 0) s_menu_sel--; menu_slide(); }
            else if (i == BTN_RIGHT_ID) { if (s_menu_sel < N_APPS - 1) s_menu_sel++; menu_slide(); }
            else { menu_leave(); open_app(s_menu_sel); }
            break;
        case SCR_APP:
            if (s_app && s_app->on_btn) s_app->on_btn(i);
            break;
        }
    }
    if (all_released) s_bl_swallow = false;   /* відпустили — знімаємо «ковтання» */
}

/* ---------------- main ---------------- */

static uint32_t tick_cb(void) { return esp_timer_get_time() / 1000; }

void app_main(void)
{
    display_init();

    const int btns[] = { BTN_LEFT, BTN_MID, BTN_RIGHT };
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(btns[i]);
        gpio_set_direction(btns[i], GPIO_MODE_INPUT);
        gpio_pullup_en(btns[i]);
    }

    lv_init();
    lv_tick_set_cb(tick_cb);
    s_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(s_disp, flush_cb);
    static uint8_t *b1, *b2;
    size_t sz = LCD_W * BUF_LINES * 2;
    /* буфери у ВНУТРІШНІЙ DMA-RAM: PSRAM-DMA зависає, коли активний BLE
       (контролер вимикає кеш, і читання з PSRAM стопориться посеред SPI-DMA).
       Wi-Fi і BLE ніколи не працюють разом, тож внутрішньої RAM вистачає. */
    b1 = heap_caps_malloc(sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    b2 = heap_caps_malloc(sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    lv_display_set_buffers(s_disp, b1, b2, sz, LV_DISPLAY_RENDER_MODE_PARTIAL);

    audio_init();             /* спільний аудіотракт (радіо + TTS Gemini) */
    battery_init();
    led_init();
    netcfg_init();
    settings_init();          /* застосувати збережені яскравість/гучність */

    /* переносимо збережену мережу у список (для позначки), без підключення */
    char ssid[33], pass[65];
    if (netcfg_load(ssid, sizeof(ssid), pass, sizeof(pass)))
        netcfg_save(ssid, pass);

    show_home();                 /* показує обличчя й вимикає Wi-Fi (сон) */
    lv_refr_now(s_disp);

    lv_timer_create(buttons_poll_cb, 25, NULL);

    while (1) {
        uint32_t d = lv_timer_handler();
        if (d > 40) d = 40;
        if (d < 5) d = 5;
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

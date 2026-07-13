/*
 * Модулі: універсальна колодка на 4-пін роз'ємі (GND/VCC/GPIO12/GPIO11).
 * GPIO11 = SDA, GPIO12 = SCL (I2C), або одиничний GPIO для вводу/виводу.
 * Підменю з типами модулів: BME280, I2C-сканер, цифровий вихід, аналог. вхід.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "lvgl.h"
#include "apps.h"

#define PIN_SDA 11
#define PIN_SCL 12

typedef enum { M_MENU, M_BME, M_SCAN, M_OUT, M_ADC } mstate_t;
static mstate_t s_st;
static int s_sel;

static i2c_master_bus_handle_t s_bus;
static bool s_bus_ok;

static lv_obj_t *s_scr, *s_root;
static lv_obj_t *s_items[4];
static lv_timer_t *s_timer;

static const char *ITEMS[] = {
    "BME280 (клімат)", "I2C-сканер", "Цифровий вихід", "Аналоговий вхід"
};
#define N_ITEMS 4

/* ---------------- I2C ---------------- */

static void i2c_up(void)
{
    if (s_bus_ok) return;
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    s_bus_ok = (i2c_new_master_bus(&cfg, &s_bus) == ESP_OK);
}
static void i2c_down(void)
{
    if (s_bus_ok) { i2c_del_master_bus(s_bus); s_bus_ok = false; }
}

/* ---------------- BME280 ---------------- */

static i2c_master_dev_handle_t s_bme;
static uint8_t s_bme_addr;
/* калібрувальні коефіцієнти */
static uint16_t T1; static int16_t T2, T3;
static uint16_t P1; static int16_t P2, P3, P4, P5, P6, P7, P8, P9;
static uint8_t  H1, H3; static int16_t H2, H4, H5; static int8_t H6;
static int32_t t_fine;

static bool bme_rd(uint8_t reg, uint8_t *buf, int len)
{
    return i2c_master_transmit_receive(s_bme, &reg, 1, buf, len, 200) == ESP_OK;
}
static bool bme_wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_bme, b, 2, 200) == ESP_OK;
}

static bool bme_init(void)
{
    i2c_up();
    if (!s_bus_ok) return false;
    /* пробуємо адреси 0x76 і 0x77 */
    for (int a = 0x76; a <= 0x77; a++) {
        if (i2c_master_probe(s_bus, a, 100) != ESP_OK) continue;
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = a, .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_bme) != ESP_OK) continue;
        uint8_t id = 0;
        if (bme_rd(0xD0, &id, 1) && id == 0x60) { s_bme_addr = a; break; }
        i2c_master_bus_rm_device(s_bme); s_bme = NULL;
    }
    if (!s_bme) return false;

    uint8_t c[26];
    bme_rd(0x88, c, 26);
    T1 = c[0] | (c[1] << 8); T2 = c[2] | (c[3] << 8); T3 = c[4] | (c[5] << 8);
    P1 = c[6] | (c[7] << 8); P2 = c[8] | (c[9] << 8); P3 = c[10] | (c[11] << 8);
    P4 = c[12] | (c[13] << 8); P5 = c[14] | (c[15] << 8); P6 = c[16] | (c[17] << 8);
    P7 = c[18] | (c[19] << 8); P8 = c[20] | (c[21] << 8); P9 = c[22] | (c[23] << 8);
    H1 = c[25];
    uint8_t h[7];
    bme_rd(0xE1, h, 7);
    H2 = h[0] | (h[1] << 8); H3 = h[2];
    H4 = (h[3] << 4) | (h[4] & 0x0F);
    H5 = (h[5] << 4) | (h[4] >> 4);
    H6 = (int8_t)h[6];

    bme_wr(0xF2, 0x01);        /* вологість x1 */
    bme_wr(0xF4, 0x27);        /* темп x1, тиск x1, нормальний режим */
    bme_wr(0xF5, 0xA0);        /* standby 1s */
    return true;
}

static bool bme_read(float *temp, float *hum, float *press)
{
    uint8_t d[8];
    if (!bme_rd(0xF7, d, 8)) return false;
    int32_t adc_P = ((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4);
    int32_t adc_T = ((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4);
    int32_t adc_H = ((uint32_t)d[6] << 8) | d[7];

    /* температура */
    double v1 = (((double)adc_T) / 16384.0 - ((double)T1) / 1024.0) * (double)T2;
    double v2 = ((((double)adc_T) / 131072.0 - ((double)T1) / 8192.0) *
                 (((double)adc_T) / 131072.0 - ((double)T1) / 8192.0)) * (double)T3;
    t_fine = (int32_t)(v1 + v2);
    *temp = (v1 + v2) / 5120.0;

    /* тиск */
    double p1 = ((double)t_fine / 2.0) - 64000.0;
    double p2 = p1 * p1 * (double)P6 / 32768.0;
    p2 = p2 + p1 * (double)P5 * 2.0;
    p2 = (p2 / 4.0) + ((double)P4 * 65536.0);
    p1 = ((double)P3 * p1 * p1 / 524288.0 + (double)P2 * p1) / 524288.0;
    p1 = (1.0 + p1 / 32768.0) * (double)P1;
    double pr = 0;
    if (p1 != 0) {
        pr = 1048576.0 - (double)adc_P;
        pr = (pr - (p2 / 4096.0)) * 6250.0 / p1;
        p1 = (double)P9 * pr * pr / 2147483648.0;
        p2 = pr * (double)P8 / 32768.0;
        pr = pr + (p1 + p2 + (double)P7) / 16.0;
    }
    *press = pr / 100.0;       /* гПа */

    /* вологість */
    double h = ((double)t_fine - 76800.0);
    h = ((double)adc_H - ((double)H4 * 64.0 + (double)H5 / 16384.0 * h)) *
        ((double)H2 / 65536.0 * (1.0 + (double)H6 / 67108864.0 * h *
        (1.0 + (double)H3 / 67108864.0 * h)));
    h = h * (1.0 - (double)H1 * h / 524288.0);
    if (h > 100) h = 100; else if (h < 0) h = 0;
    *hum = h;
    return true;
}

/* ---------------- цифровий вихід / аналог ---------------- */

static bool s_out_state;
static adc_oneshot_unit_handle_t s_adc;
static bool s_adc_ok;

static void out_up(void)
{
    gpio_reset_pin(PIN_SDA);
    gpio_set_direction(PIN_SDA, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_SDA, s_out_state);
}
static void adc_up(void)
{
    if (s_adc_ok) return;
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_2 };
    if (adc_oneshot_new_unit(&u, &s_adc) != ESP_OK) return;
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    /* GPIO11 = ADC2 канал 0 */
    if (adc_oneshot_config_channel(s_adc, ADC_CHANNEL_0, &ch) == ESP_OK) s_adc_ok = true;
}
static void adc_down(void)
{
    if (s_adc_ok) { adc_oneshot_del_unit(s_adc); s_adc_ok = false; }
}

/* ---------------- звільнення периферії ---------------- */

static void modules_release(void)
{
    if (s_bme) { i2c_master_bus_rm_device(s_bme); s_bme = NULL; }
    i2c_down();
    adc_down();
    gpio_reset_pin(PIN_SDA);
    gpio_reset_pin(PIN_SCL);
}

/* ---------------- UI ---------------- */

static void title(const char *t)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, &font_ua_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 8);
}

static void kv(const char *k, const char *v, uint32_t vcol)
{
    lv_obj_t *row = lv_obj_create(s_root);
    lv_obj_set_size(row, 216, 30);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161D26), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 5, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *a = lv_label_create(row);
    lv_label_set_text(a, k);
    lv_obj_set_style_text_color(a, lv_color_hex(0x8A94A0), 0);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *b = lv_label_create(row);
    lv_label_set_text(b, v);
    lv_obj_set_style_text_color(b, lv_color_hex(vcol), 0);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
}

static lv_obj_t *make_root(void)
{
    lv_obj_t *r = lv_obj_create(s_scr);
    lv_obj_set_size(r, 226, 190);
    lv_obj_align(r, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(r, LV_OPA_0, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 2, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r, 6, 0);
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_ACTIVE);
    return r;
}

/* --- підменю --- */
static void menu_hl(void)
{
    for (int i = 0; i < N_ITEMS; i++)
        lv_obj_set_style_bg_color(s_items[i],
            lv_color_hex(i == s_sel ? 0x2563EB : 0x1A222C), 0);
}
static void show_menu(void)
{
    s_st = M_MENU;
    lv_obj_clean(s_scr);
    title("Модулі");
    lv_obj_t *hint = lv_label_create(s_scr);
    lv_label_set_text(hint, "роз'єм GND/VCC/12/11");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 34);
    for (int i = 0; i < N_ITEMS; i++) {
        lv_obj_t *b = lv_obj_create(s_scr);
        lv_obj_set_size(b, 210, 40);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 56 + i * 46);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_scrollbar_mode(b, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, ITEMS[i]);
        lv_obj_set_style_text_color(l, lv_color_hex(0xE8ECF0), 0);
        lv_obj_center(l);
        s_items[i] = b;
    }
    menu_hl();
}

/* --- BME280 view --- */
static bool s_bme_present;
static void bme_render(void)
{
    lv_obj_clean(s_root);
    if (!s_bme_present) {
        lv_obj_t *l = lv_label_create(s_root);
        lv_label_set_text(l, "BME280 не знайдено.\nПеревірте підключення\n(11=SDA, 12=SCL)");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xF87171), 0);
        return;
    }
    float t, h, p;
    if (!bme_read(&t, &h, &p)) return;
    char b[32];
    snprintf(b, sizeof(b), "%.1f°C", t);   kv("Температура", b, 0x35C4F0);
    snprintf(b, sizeof(b), "%.0f%%", h);    kv("Вологість", b, 0x4ADE80);
    snprintf(b, sizeof(b), "%.0f гПа", p);  kv("Тиск", b, 0xE8ECF0);
    snprintf(b, sizeof(b), "%.0f мм рт", p * 0.750062); kv("", b, 0x8A94A0);
    snprintf(b, sizeof(b), "0x%02X", s_bme_addr); kv("Адреса I2C", b, 0x5A6672);
}

/* --- I2C scan --- */
static void scan_render(void)
{
    lv_obj_clean(s_root);
    i2c_up();
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_bus, a, 30) == ESP_OK) {
            char b[24];
            snprintf(b, sizeof(b), "0x%02X", a);
            const char *known = a == 0x76 || a == 0x77 ? "BME/BMP280" :
                                a == 0x68 ? "MPU/RTC?" :
                                a == 0x3C || a == 0x3D ? "OLED?" : "—";
            kv(b, known, 0x4ADE80);
            found++;
        }
    }
    if (!found) {
        lv_obj_t *l = lv_label_create(s_root);
        lv_label_set_text(l, "Пристроїв не знайдено.\nЦентр — сканувати ще");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x8A94A0), 0);
    }
}

/* --- цифровий вихід --- */
static void out_render(void)
{
    lv_obj_clean(s_root);
    out_up();
    char b[16];
    snprintf(b, sizeof(b), s_out_state ? "УВІМК (3.3В)" : "ВИМК (0В)");
    kv("GPIO11", b, s_out_state ? 0x4ADE80 : 0x5A6672);
    lv_obj_t *h = lv_label_create(s_root);
    lv_label_set_text(h, "центр — перемкнути");
    lv_obj_set_style_text_color(h, lv_color_hex(0x3A4550), 0);
}

/* --- аналоговий вхід --- */
static void adc_render(void)
{
    lv_obj_clean(s_root);
    adc_up();
    if (!s_adc_ok) {
        lv_obj_t *l = lv_label_create(s_root);
        lv_label_set_text(l, "ADC недоступний");
        lv_obj_set_style_text_color(l, lv_color_hex(0xF87171), 0);
        return;
    }
    int v = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_0, &v) != ESP_OK) return;
    char b[24];
    snprintf(b, sizeof(b), "%d / 4095", v); kv("Значення", b, 0x35C4F0);
    snprintf(b, sizeof(b), "%.2f В", v / 4095.0 * 3.3); kv("Напруга", b, 0x4ADE80);
    kv("Пін", "GPIO11 (ADC2)", 0x5A6672);
}
static void mod_tick(lv_timer_t *t)
{
    static int c = 0; c++;
    if (s_st == M_ADC) adc_render();                 /* 0.5 с */
    else if (s_st == M_BME && (c % 3) == 0) bme_render(); /* ~1.5 с */
}

/* ---------------- перемикання ---------------- */

static void open_view(int idx)
{
    lv_obj_clean(s_scr);
    s_root = make_root();
    switch (idx) {
    case 0: s_st = M_BME; title("BME280");
        s_bme_present = bme_init(); bme_render(); break;
    case 1: s_st = M_SCAN; title("I2C-сканер"); scan_render(); break;
    case 2: s_st = M_OUT; title("Цифровий вихід"); out_render(); break;
    case 3: s_st = M_ADC; title("Аналоговий вхід"); adc_render(); break;
    }
}

static void mod_open(lv_obj_t *scr)
{
    s_scr = scr;
    s_sel = 0;
    show_menu();
    s_timer = lv_timer_create(mod_tick, 500, NULL);  /* один спільний таймер */
}

static void mod_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    modules_release();
}

static void mod_btn(int btn)
{
    if (s_st == M_MENU) {
        if (btn == BTN_LEFT_ID) { if (s_sel > 0) s_sel--; menu_hl(); }
        else if (btn == BTN_RIGHT_ID) { if (s_sel < N_ITEMS - 1) s_sel++; menu_hl(); }
        else open_view(s_sel);
    } else {
        if (btn == BTN_MID_ID) {
            if (s_st == M_SCAN) scan_render();
            else if (s_st == M_OUT) { s_out_state = !s_out_state; out_render(); }
            else { modules_release(); show_menu(); }   /* BME/ADC — назад у підменю */
        }
    }
}

const app_t app_modules = {
    .name = "Модулі",
    .open = mod_open, .close = mod_close, .on_btn = mod_btn,
};

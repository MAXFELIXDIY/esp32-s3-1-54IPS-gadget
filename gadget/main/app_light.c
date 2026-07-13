/*
 * Нічник / ліхтарик: керування RGB-світлодіодом як лампою.
 * Ліва/права — колір, центр — яскравість (3 рівні). Стан лишається після
 * виходу (світиться, як нічник), доки не вимкнути кольором «Вимк».
 */
#include <stdio.h>
#include "lvgl.h"
#include "apps.h"
#include "led.h"

typedef struct { const char *name; uint8_t r, g, b; } color_t;
static const color_t COLORS[] = {
    { "Вимк",      0,   0,   0 },
    { "Тепле",     255, 140, 40 },
    { "Біле",      255, 255, 255 },
    { "Червоне",   255, 0,   0 },
    { "Помаранч.", 255, 90,  0 },
    { "Зелене",    0,   255, 0 },
    { "Блакитне",  0,   180, 255 },
    { "Синє",      0,   0,   255 },
    { "Фіолетове", 200, 0,   255 },
};
#define N_COL (int)(sizeof(COLORS) / sizeof(COLORS[0]))
static const int BRI[] = { 15, 45, 100 };   /* % */

static int s_ci = 1, s_bi = 1;
static lv_obj_t *s_scr, *s_swatch, *s_name, *s_bri;

static void apply(void)
{
    const color_t *c = &COLORS[s_ci];
    int b = BRI[s_bi];
    led_set(c->r * b / 100, c->g * b / 100, c->b * b / 100);

    /* прев'ю на екрані (у повній яскравості для видимості) */
    lv_obj_set_style_bg_color(s_swatch, lv_color_make(c->r, c->g, c->b), 0);
    lv_obj_set_style_bg_opa(s_swatch, s_ci == 0 ? LV_OPA_20 : LV_OPA_COVER, 0);
    lv_label_set_text(s_name, c->name);
    if (s_ci == 0) lv_label_set_text(s_bri, "");
    else lv_label_set_text_fmt(s_bri, "яскравість %d%%", b);
}

static void light_open(lv_obj_t *scr)
{
    s_scr = scr;
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Нічник");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_swatch = lv_obj_create(scr);
    lv_obj_set_size(s_swatch, 120, 120);
    lv_obj_align(s_swatch, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(s_swatch, 60, 0);
    lv_obj_set_style_border_width(s_swatch, 2, 0);
    lv_obj_set_style_border_color(s_swatch, lv_color_hex(0x2A333D), 0);

    s_name = lv_label_create(scr);
    lv_obj_set_style_text_font(s_name, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_name, lv_color_hex(0xE8ECF0), 0);
    lv_obj_align(s_name, LV_ALIGN_BOTTOM_MID, 0, -46);

    s_bri = lv_label_create(scr);
    lv_obj_set_style_text_color(s_bri, lv_color_hex(0x8A94A0), 0);
    lv_obj_align(s_bri, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "< > колір   центр — яскравість");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    apply();
}

static void light_close(void)
{
    /* навмисно НЕ гасимо — нічник лишається світити після виходу */
}

static void light_btn(int btn)
{
    if (btn == BTN_LEFT_ID) s_ci = (s_ci + N_COL - 1) % N_COL;
    else if (btn == BTN_RIGHT_ID) s_ci = (s_ci + 1) % N_COL;
    else s_bi = (s_bi + 1) % 3;
    apply();
}

const app_t app_light = {
    .name = "Нічник",
    .open = light_open, .close = light_close, .on_btn = light_btn,
};

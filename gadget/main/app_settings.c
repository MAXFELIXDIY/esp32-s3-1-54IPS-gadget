/*
 * Налаштування: підменю (Wi-Fi, Яскравість, Гучність).
 * Wi-Fi делегується застосунку app_wifi. Повзунки: ліва/права — змінити
 * (клік = крок, затиск = швидко через автоповтор), центр — назад у підменю.
 * Утримання центра — вихід у головне меню (обробляє каркас).
 */
#include <stdio.h>
#include "lvgl.h"
#include "apps.h"
#include "settings.h"

typedef enum { S_MENU, S_WIFI, S_BRIGHT, S_VOL } sub_t;
static sub_t s_sub;
static int s_sel;

static lv_obj_t *s_scr;
static lv_obj_t *s_items[3];
static lv_obj_t *s_bar, *s_val;

static const char *ITEMS[] = { "Wi-Fi", "Яскравість", "Гучність" };
#define N_ITEMS 3

/* ---------- підменю ---------- */

static void menu_hl(void)
{
    for (int i = 0; i < N_ITEMS; i++)
        lv_obj_set_style_bg_color(s_items[i],
            lv_color_hex(i == s_sel ? 0x2563EB : 0x1A222C), 0);
}

static void show_menu(void)
{
    s_sub = S_MENU;
    lv_obj_clean(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0B0F14), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Налаштування");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    for (int i = 0; i < N_ITEMS; i++) {
        lv_obj_t *b = lv_obj_create(s_scr);
        lv_obj_set_size(b, 200, 46);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 52 + i * 54);
        lv_obj_set_style_radius(b, 10, 0);
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

/* ---------- повзунок ---------- */

static void show_slider(const char *name, int vmin, int vmax, int val)
{
    lv_obj_clean(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0B0F14), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, name);
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    s_val = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_val, &font_num_34, 0);
    lv_obj_set_style_text_color(s_val, lv_color_hex(0x35C4F0), 0);
    lv_obj_align(s_val, LV_ALIGN_CENTER, 0, -10);

    s_bar = lv_bar_create(s_scr);
    lv_obj_set_size(s_bar, 190, 18);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 40);
    lv_bar_set_range(s_bar, vmin, vmax);
    lv_bar_set_value(s_bar, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x1A222C), 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x35C4F0), LV_PART_INDICATOR);

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_label_set_text(hint, "< >  змінити    центр — готово");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void bright_update(void)
{
    lv_bar_set_value(s_bar, settings_brightness(), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_val, "%d%%", settings_brightness() * 100 / 255);
}
static void vol_update(void)
{
    lv_bar_set_value(s_bar, settings_volume(), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_val, "%d%%", settings_volume() * 100 / 256);
}

/* ---------- app ---------- */

static void sett_open(lv_obj_t *scr)
{
    s_scr = scr;
    s_sel = 0;
    show_menu();
}

static void sett_close(void)
{
    if (s_sub == S_WIFI) app_wifi.close();
}

static void sett_btn(int btn)
{
    switch (s_sub) {
    case S_MENU:
        if (btn == BTN_LEFT_ID) { s_sel = (s_sel + N_ITEMS - 1) % N_ITEMS; menu_hl(); }
        else if (btn == BTN_RIGHT_ID) { s_sel = (s_sel + 1) % N_ITEMS; menu_hl(); }
        else {
            if (s_sel == 0) {                    /* Wi-Fi */
                s_sub = S_WIFI;
                lv_obj_clean(s_scr);
                app_wifi.open(s_scr);
            } else if (s_sel == 1) {             /* Яскравість */
                s_sub = S_BRIGHT;
                show_slider("Яскравість", 15, 255, settings_brightness());
                bright_update();
            } else {                             /* Гучність */
                s_sub = S_VOL;
                show_slider("Гучність", 0, 256, settings_volume());
                vol_update();
            }
        }
        break;

    case S_WIFI:
        app_wifi.on_btn(btn);
        break;

    case S_BRIGHT:
        if (btn == BTN_LEFT_ID) settings_set_brightness(settings_brightness() - 12);
        else if (btn == BTN_RIGHT_ID) settings_set_brightness(settings_brightness() + 12);
        else { show_menu(); break; }
        bright_update();
        break;

    case S_VOL:
        if (btn == BTN_LEFT_ID) settings_set_volume(settings_volume() - 16);
        else if (btn == BTN_RIGHT_ID) settings_set_volume(settings_volume() + 16);
        else { show_menu(); break; }
        vol_update();
        break;
    }
}

const app_t app_settings = {
    .name = "Налаштування",
    .open = sett_open,
    .close = sett_close,
    .on_btn = sett_btn,
};

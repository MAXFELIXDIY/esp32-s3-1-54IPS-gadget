/*
 * Налаштування: підменю (Wi-Fi, Яскравість, Гучність).
 * Wi-Fi делегується застосунку app_wifi. Повзунки: ліва/права — змінити
 * (клік = крок, затиск = швидко через автоповтор), центр — назад у підменю.
 * Утримання центра — вихід у головне меню (обробляє каркас).
 */
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "lvgl.h"
#include "apps.h"
#include "settings.h"

typedef enum { S_MENU, S_WIFI, S_BRIGHT, S_VOL, S_CUSTOM, S_RES } sub_t;
static sub_t s_sub;
static int s_sel;

static lv_obj_t *s_scr;
static lv_obj_t *s_items[5];
static lv_obj_t *s_bar, *s_val;

static const char *ITEMS[] = { "Wi-Fi", "Яскравість", "Гучність",
                               "Кастомізація меню", "Монітор ресурсів" };
#define N_ITEMS 5

/* --- кастомізація меню --- */
#define MAXAPPS 24
static lv_obj_t *s_crows[MAXAPPS], *s_cdots[MAXAPPS];
static int s_ccount, s_csel;

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
        lv_obj_set_size(b, 212, 36);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 42 + i * 39);
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

/* ---------- кастомізація меню ---------- */

static void custom_dot(int i)      /* колір індикатора: увімк./вимк. */
{
    int idx = i;                   /* рядок = індекс застосунку */
    bool on = menu_app_locked(idx) || settings_menu_visible(idx);
    lv_obj_set_style_bg_color(s_cdots[i],
        lv_color_hex(on ? 0x4ADE80 : 0x3A4550), 0);
}

static void custom_hl(void)
{
    for (int i = 0; i < s_ccount; i++)
        lv_obj_set_style_bg_color(s_crows[i],
            lv_color_hex(i == s_csel ? 0x2563EB : 0x161D26), 0);
    if (s_csel < s_ccount) lv_obj_scroll_to_view(s_crows[s_csel], LV_ANIM_ON);
}

static void show_custom(void)
{
    s_sub = S_CUSTOM;
    s_csel = 0;
    lv_obj_clean(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0B0F14), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Кастомізація меню");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *list = lv_obj_create(s_scr);
    lv_obj_set_size(list, 228, 190);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 5, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);

    s_ccount = menu_app_count();
    if (s_ccount > MAXAPPS) s_ccount = MAXAPPS;
    for (int i = 0; i < s_ccount; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, 214, 34);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        s_crows[i] = row;

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, menu_app_name(i));
        lv_obj_set_width(nm, 165);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(nm,
            lv_color_hex(menu_app_locked(i) ? 0x8A94A0 : 0xE8ECF0), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *dot = lv_obj_create(row);   /* індикатор увімк./вимк. */
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, 0, 0);
        s_cdots[i] = dot;
        custom_dot(i);
    }
    custom_hl();

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_label_set_text(hint, "центр — увімк/вимк");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

/* ---------- монітор ресурсів ---------- */

static lv_obj_t *s_res_val[4];   /* значення показників */
static lv_timer_t *s_res_timer;

/* реальний розмір образу прошивки в app-розділі (сума сегментів) */
static size_t app_used_bytes(const esp_partition_t *p)
{
    if (!p) return 0;
    esp_image_header_t h;
    if (esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return 0;
    size_t off = sizeof(h);
    for (int i = 0; i < h.segment_count && i < 16; i++) {
        esp_image_segment_header_t s;
        if (esp_partition_read(p, off, &s, sizeof(s)) != ESP_OK) return 0;
        off += sizeof(s) + s.data_len;
    }
    off += 1;                       /* байт контрольної суми */
    off = (off + 15) & ~15u;        /* вирівнювання до 16 */
    if (h.hash_appended) off += 32; /* SHA-256 у хвості */
    return off;
}

/* зайнято флешу = найвищий кінець серед усіх розділів (розмітка) */
static size_t flash_used_bytes(void)
{
    size_t maxend = 0;
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it) {
        const esp_partition_t *p = esp_partition_get(it);
        size_t e = p->address + p->size;
        if (e > maxend) maxend = e;
        it = esp_partition_next(it);
    }
    return maxend;
}

static const char *RES_NM[4] = { "RAM (внутр.)", "PSRAM", "Прошивка", "Flash (чіп)" };
static const uint32_t RES_COL[4] = { 0x4ADE80, 0x35C4F0, 0xFACC15, 0xF59E42 };

static void res_update(void)
{
    size_t ri_t = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t ri_f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t rp_t = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t rp_f = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t flash = 0; esp_flash_get_size(NULL, &flash);
    const esp_partition_t *run = esp_ota_get_running_partition();
    size_t app_t = run ? run->size : 0;

    lv_label_set_text_fmt(s_res_val[0], "%u / %u kB",
        (unsigned)((ri_t - ri_f) / 1024), (unsigned)(ri_t / 1024));
    lv_label_set_text_fmt(s_res_val[1], "%u / %u kB",
        (unsigned)((rp_t - rp_f) / 1024), (unsigned)(rp_t / 1024));
    lv_label_set_text_fmt(s_res_val[2], "%u / %u kB",
        (unsigned)(app_used_bytes(run) / 1024), (unsigned)(app_t / 1024));
    lv_label_set_text_fmt(s_res_val[3], "%u / %u kB",
        (unsigned)(flash_used_bytes() / 1024), (unsigned)(flash / 1024));
}

static void res_tick(lv_timer_t *t) { res_update(); }

static void show_res(void)
{
    s_sub = S_RES;
    if (s_res_timer) { lv_timer_delete(s_res_timer); s_res_timer = NULL; }
    lv_obj_clean(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0B0F14), 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "Монітор ресурсів");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    for (int i = 0; i < 4; i++) {
        int y = 36 + i * 44;
        lv_obj_t *nm = lv_label_create(s_scr);
        lv_label_set_text(nm, RES_NM[i]);
        lv_obj_set_style_text_font(nm, &font_ua_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(RES_COL[i]), 0);
        lv_obj_set_pos(nm, 14, y);

        lv_obj_t *v = lv_label_create(s_scr);
        lv_obj_set_style_text_font(v, &font_ua_16, 0);
        lv_obj_set_style_text_color(v, lv_color_hex(RES_COL[i]), 0);
        lv_obj_set_pos(v, 14, y + 18);
        s_res_val[i] = v;

        if (i < 3) {                          /* розділювальна лінія свого кольору */
            lv_obj_t *ln = lv_obj_create(s_scr);
            lv_obj_set_size(ln, 210, 2);
            lv_obj_set_pos(ln, 14, y + 40);
            lv_obj_set_style_radius(ln, 1, 0);
            lv_obj_set_style_bg_color(ln, lv_color_hex(RES_COL[i]), 0);
            lv_obj_set_style_bg_opa(ln, 90, 0);
            lv_obj_set_style_border_width(ln, 0, 0);
            lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
    res_update();
    s_res_timer = lv_timer_create(res_tick, 1000, NULL);   /* «живе» оновлення */

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_label_set_text(hint, "центр — назад");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
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
    if (s_res_timer) { lv_timer_delete(s_res_timer); s_res_timer = NULL; }
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
            } else if (s_sel == 2) {             /* Гучність */
                s_sub = S_VOL;
                show_slider("Гучність", 0, 256, settings_volume());
                vol_update();
            } else if (s_sel == 3) {             /* Кастомізація меню */
                show_custom();
            } else {                             /* Монітор ресурсів */
                show_res();
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

    case S_CUSTOM:
        if (btn == BTN_LEFT_ID) { if (s_csel > 0) s_csel--; custom_hl(); }
        else if (btn == BTN_RIGHT_ID) { if (s_csel < s_ccount - 1) s_csel++; custom_hl(); }
        else {                                   /* центр — увімк/вимк */
            if (!menu_app_locked(s_csel)) {
                settings_menu_toggle(s_csel);
                custom_dot(s_csel);
            }
        }
        break;

    case S_RES:
        if (btn == BTN_MID_ID) {                 /* центр — назад */
            if (s_res_timer) { lv_timer_delete(s_res_timer); s_res_timer = NULL; }
            show_menu();
        }
        break;
    }
}

const app_t app_settings = {
    .name = "Налаштування",
    .open = sett_open,
    .close = sett_close,
    .on_btn = sett_btn,
};

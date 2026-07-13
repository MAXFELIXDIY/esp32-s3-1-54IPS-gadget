/*
 * Wi-Fi: сканування мереж → вибір → екранна клавіатура на 3 кнопки для
 * пароля (ліва/права — символ, середня — активувати) → підключення +
 * збереження в NVS. Спецсимволи ⌫ (стерти) і OK (підключитись) — частина
 * набору. Середня довго — вихід у меню (обробляє каркас).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"

#define MAX_APS 20

/* набір символів + 2 спецкнопки в кінці */
static const char CHARSET[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    "!@#$%^&*()-_=+.,:;?/ ";
#define N_CHARS ((int)sizeof(CHARSET) - 1)
#define KEY_BKSP (N_CHARS)      /* ⌫ */
#define KEY_OK   (N_CHARS + 1)  /* OK */
#define N_KEYS   (N_CHARS + 2)

typedef enum { V_LIST, V_KEY, V_CONN } view_t;
static view_t s_view;

static wifi_ap_record_t s_aps[MAX_APS];
static int s_n, s_sel;
static char s_ssid[33];
static char s_pass[65];
static int s_plen;
static int s_key;

static lv_obj_t *s_root;
static lv_obj_t *s_rows[MAX_APS];
static lv_obj_t *s_cells[N_KEYS];
static lv_obj_t *s_kb_pass, *s_kb_ssid;
static lv_timer_t *s_conn_timer;
#define KB_COLS 7
#define KB_CELL 28

/* ---------- список мереж ---------- */

static void list_render(void)
{
    lv_obj_clean(s_root);
    for (int i = 0; i < s_n; i++) {
        lv_obj_t *row = lv_obj_create(s_root);
        lv_obj_set_size(row, 208, 44);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row,
            lv_color_hex(i == s_sel ? 0x2563EB : 0x161D26), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        s_rows[i] = row;

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, (char *)s_aps[i].ssid);
        lv_obj_set_width(nm, 150);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *ic = lv_label_create(row);
        bool open = s_aps[i].authmode == WIFI_AUTH_OPEN;
        lv_label_set_text_fmt(ic, "%s %d", open ? "" : "*",
                              s_aps[i].rssi);
        lv_obj_set_style_text_color(ic, lv_color_hex(0x5A6672), 0);
        lv_obj_align(ic, LV_ALIGN_RIGHT_MID, 0, 0);

        /* зелена позначка для збережених мереж */
        if (netcfg_has_saved((char *)s_aps[i].ssid)) {
            lv_obj_t *ck = lv_label_create(row);
            lv_label_set_text(ck, "\xE2\x80\xA2");   /* • (є у шрифті) */
            lv_obj_set_style_text_color(ck, lv_color_hex(0x4ADE80), 0);
            lv_obj_align(ck, LV_ALIGN_RIGHT_MID, -46, 0);
        }
    }
    if (s_sel < s_n) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
}

static void do_scan(void)
{
    lv_obj_clean(s_root);
    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, "Сканування...");
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A94A0), 0);
    lv_obj_center(l);
    lv_refr_now(NULL);

    s_n = netcfg_scan(s_aps, MAX_APS);
    s_sel = 0;
    if (s_n == 0) {
        lv_obj_clean(s_root);
        lv_obj_t *e = lv_label_create(s_root);
        lv_label_set_text(e, "Мереж не знайдено.\nСередня — ще раз.");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(e, lv_color_hex(0xF87171), 0);
        lv_obj_center(e);
    } else {
        list_render();
    }
}

/* ---------- клавіатура ---------- */

static void kb_render(void)
{
    for (int i = 0; i < N_KEYS; i++) {
        uint32_t bg = 0x161D26;
        if (i == KEY_OK)   bg = (i == s_key) ? 0x22C55E : 0x14532D;
        else if (i == KEY_BKSP) bg = (i == s_key) ? 0xF59E0B : 0x5A3A0A;
        else if (i == s_key) bg = 0x2563EB;
        lv_obj_set_style_bg_color(s_cells[i], lv_color_hex(bg), 0);
    }
    lv_obj_scroll_to_view(s_cells[s_key], LV_ANIM_ON);
    lv_label_set_text(s_kb_pass, s_plen ? s_pass : "(введіть пароль)");
}

static void show_keyboard(void)
{
    s_view = V_KEY;
    s_plen = 0; s_pass[0] = 0; s_key = 0;
    lv_obj_clean(s_root);

    s_kb_ssid = lv_label_create(s_root);
    lv_label_set_text_fmt(s_kb_ssid, "Пароль до %s:", s_ssid);
    lv_obj_set_width(s_kb_ssid, 224);
    lv_label_set_long_mode(s_kb_ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_kb_ssid, lv_color_hex(0x8A94A0), 0);
    lv_obj_align(s_kb_ssid, LV_ALIGN_TOP_LEFT, 2, 0);

    s_kb_pass = lv_label_create(s_root);
    lv_obj_set_width(s_kb_pass, 224);
    lv_label_set_long_mode(s_kb_pass, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_kb_pass, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_kb_pass, LV_ALIGN_TOP_LEFT, 2, 18);

    /* сітка символів */
    lv_obj_t *grid = lv_obj_create(s_root);
    lv_obj_set_size(grid, 232, 118);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_bg_opa(grid, LV_OPA_0, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 2, 0);
    lv_obj_set_style_pad_row(grid, 3, 0);
    lv_obj_set_style_pad_column(grid, 3, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_ACTIVE);

    for (int i = 0; i < N_KEYS; i++) {
        lv_obj_t *cell = lv_obj_create(grid);
        int w = (i >= N_CHARS) ? KB_CELL * 2 + 3 : KB_CELL; /* спец — ширші */
        lv_obj_set_size(cell, w, KB_CELL);
        lv_obj_set_style_radius(cell, 5, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_set_scrollbar_mode(cell, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *l = lv_label_create(cell);
        if (i == KEY_BKSP) lv_label_set_text(l, "Стерти");
        else if (i == KEY_OK) lv_label_set_text(l, "OK");
        else {
            char c[2] = { CHARSET[i], 0 };
            lv_label_set_text(l, c[0] == ' ' ? "␣" : c);
        }
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(l);
        s_cells[i] = cell;
    }

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, "< > курсор    центр — вибір");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    kb_render();
}

/* ---------- підключення ---------- */

static volatile int s_conn_result;  /* 0 очікування, 1 ок, 2 помилка */

static void show_connecting(void)
{
    s_view = V_CONN;
    lv_obj_clean(s_root);
    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text_fmt(l, "Підключення до\n%s ...", s_ssid);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC0C8D0), 0);
    lv_obj_center(l);
}

static void conn_task(void *arg)
{
    bool ok = netcfg_connect(s_ssid, s_pass, 15000);
    if (ok) netcfg_save(s_ssid, s_pass);
    /* результат покаже таймер (LVGL не чіпаємо з іншої задачі) */
    s_conn_result = ok ? 1 : 2;
    vTaskDelete(NULL);
}

static void conn_poll(lv_timer_t *t)
{
    if (s_conn_result == 0) return;
    lv_obj_clean(s_root);
    lv_obj_t *l = lv_label_create(s_root);
    if (s_conn_result == 1)
        lv_label_set_text_fmt(l, "Підключено!\n%s", s_ssid);
    else
        lv_label_set_text(l, "Не вдалося.\nСередня — назад до списку");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l,
        lv_color_hex(s_conn_result == 1 ? 0x4ADE80 : 0xF87171), 0);
    lv_obj_center(l);
    lv_timer_pause(t);
}

static void start_connect(void)
{
    show_connecting();
    s_conn_result = 0;
    if (!s_conn_timer) s_conn_timer = lv_timer_create(conn_poll, 300, NULL);
    else lv_timer_resume(s_conn_timer);
    xTaskCreate(conn_task, "wifi_conn", 4096, NULL, 4, NULL);
}

/* ---------- інтерфейс ---------- */

static void wifi_open(lv_obj_t *scr)
{
    s_view = V_LIST;
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 232, 202);
    lv_obj_align(s_root, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 2, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, 6, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);

    do_scan();
}

static void wifi_close(void)
{
    if (s_conn_timer) { lv_timer_delete(s_conn_timer); s_conn_timer = NULL; }
}

static void wifi_btn(int btn)
{
    if (s_view == V_LIST) {
        if (s_n == 0) { if (btn == BTN_MID_ID) do_scan(); return; }
        if (btn == BTN_LEFT_ID) { if (s_sel > 0) s_sel--; list_render(); }
        else if (btn == BTN_RIGHT_ID) { if (s_sel < s_n - 1) s_sel++; list_render(); }
        else {
            strlcpy(s_ssid, (char *)s_aps[s_sel].ssid, sizeof(s_ssid));
            char saved[65];
            if (s_aps[s_sel].authmode == WIFI_AUTH_OPEN) {
                s_pass[0] = 0;
                start_connect();
            } else if (netcfg_get_pass(s_ssid, saved, sizeof(saved))) {
                /* пароль уже збережено — конектимось одразу */
                strlcpy(s_pass, saved, sizeof(s_pass));
                start_connect();
            } else {
                show_keyboard();
            }
        }
    } else if (s_view == V_KEY) {
        if (btn == BTN_LEFT_ID) { s_key = (s_key + N_KEYS - 1) % N_KEYS; kb_render(); }
        else if (btn == BTN_RIGHT_ID) { s_key = (s_key + 1) % N_KEYS; kb_render(); }
        else {
            if (s_key == KEY_OK) { start_connect(); }
            else if (s_key == KEY_BKSP) {
                if (s_plen > 0) s_pass[--s_plen] = 0;
                kb_render();
            } else if (s_plen < 64) {
                s_pass[s_plen++] = CHARSET[s_key];
                s_pass[s_plen] = 0;
                kb_render();
            }
        }
    } else { /* V_CONN */
        if (btn == BTN_MID_ID && s_conn_result == 2) {
            if (s_conn_timer) lv_timer_pause(s_conn_timer);
            s_view = V_LIST;
            do_scan();                    /* назад до списку — перескан */
        }
    }
}

const app_t app_wifi = {
    .name = "Пошук Wi-Fi",
    .open = wifi_open,
    .close = wifi_close,
    .on_btn = wifi_btn,
};

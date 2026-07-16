/*
 * Wi-Fi інструменти: сканер точок доступу + деаутентифікація (deauth).
 *
 * Сканер: список мереж (SSID, канал, сигнал, шифрування, BSSID), сортування
 * за RSSI. Керування: –/+ курсор, центр — атака на вибрану точку.
 *
 * Деаутентифікація: широкомовно розсилає deauth/disassoc-кадри від імені
 * точки, через що клієнти відпадають від неї. Це «глушіння» — застосовувати
 * ЛИШЕ до власних мереж / у межах авторизованого тестування.
 *
 * Керування в атаці: центр — старт/стоп; ліва — назад до списку;
 * утримання центру — вихід у меню.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"

static const char *APPTAG = "app_wifitools";

#define WT_MAX 30

#define WT_CLI_MAX 16

typedef enum { WT_SCAN, WT_CLIENTS, WT_ATTACK } wt_view_t;
static wt_view_t s_view;

EXT_RAM_BSS_ATTR static wifi_ap_record_t s_aps[WT_MAX];
static int s_n, s_sel;
static wifi_ap_record_t s_target;

/* список клієнтів обраної точки */
static uint8_t s_cl[WT_CLI_MAX][6];
static int s_ncl, s_csel;
static uint8_t s_atk_client[6];   /* клієнт, якого зараз атакуємо */

static lv_obj_t *s_title, *s_root;
static lv_obj_t *s_rows[WT_MAX], *s_nums[WT_MAX];
static int s_built_n = -1;
static lv_timer_t *s_timer;

/* --- стан атаки (спільний з фоновою задачею) --- */
static volatile bool s_attacking = false;
static volatile bool s_attack_run = false;   /* задача жива */
static volatile uint32_t s_pkts = 0;

/* ---------- утиліти ---------- */

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "Відкрита";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "?";
    }
}

/* ---------- спінер ---------- */

static void show_spinner(const char *caption)
{
    lv_obj_clean(s_root);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sp = lv_spinner_create(s_root);
    lv_obj_set_size(sp, 48, 48);
    lv_obj_set_style_arc_color(sp, lv_color_hex(0x35C4F0), LV_PART_INDICATOR);
    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, caption);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A94A0), 0);
}

/* ---------- сканування ---------- */

static volatile bool s_scan_busy = false;
static volatile bool s_scan_done = false;

static void scan_task(void *arg)
{
    s_n = netcfg_scan(s_aps, WT_MAX);
    s_scan_busy = false;
    s_scan_done = true;
    vTaskDelete(NULL);
}

static void start_scan(void)
{
    s_view = WT_SCAN;
    s_sel = 0;
    s_built_n = -1;
    s_scan_busy = true;
    s_scan_done = false;
    lv_label_set_text(s_title, "Пошук мереж...");
    show_spinner("Сканую Wi-Fi...");
    xTaskCreate(scan_task, "wt_scan", 4096, NULL, 4, NULL);
}

/* ---------- список точок ---------- */

static void list_highlight(void)
{
    for (int i = 0; i < s_built_n; i++) {
        lv_obj_set_style_bg_color(s_rows[i],
            lv_color_hex(i == s_sel ? 0x2563EB : 0x161D26), 0);
        lv_obj_set_style_text_color(s_nums[i],
            lv_color_hex(i == s_sel ? 0xFFFFFF : 0x5A6672), 0);
    }
    if (s_sel < s_built_n) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_ON);
}

static void list_build(void)
{
    lv_obj_clean(s_root);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    for (int i = 0; i < s_n; i++) {
        lv_obj_t *row = lv_obj_create(s_root);
        lv_obj_set_size(row, 208, 46);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(i == s_sel ? 0x2563EB : 0x161D26), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        s_rows[i] = row;

        lv_obj_t *num = lv_label_create(row);
        lv_label_set_text_fmt(num, "%d", i + 1);
        lv_obj_set_style_text_color(num,
            lv_color_hex(i == s_sel ? 0xFFFFFF : 0x5A6672), 0);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 0, 0);
        s_nums[i] = num;

        lv_obj_t *nm = lv_label_create(row);
        const char *ss = (const char *)s_aps[i].ssid;
        lv_label_set_text(nm, ss[0] ? ss : "(прихована)");
        lv_obj_set_width(nm, 130);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 26, -2);

        lv_obj_t *sub = lv_label_create(row);
        lv_label_set_text_fmt(sub, "к%d • %s", s_aps[i].primary,
                              auth_name(s_aps[i].authmode));
        lv_obj_set_style_text_color(sub, lv_color_hex(0x8A94A0), 0);
        lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 26, 2);

        lv_obj_t *rs = lv_label_create(row);
        lv_label_set_text_fmt(rs, "%d", s_aps[i].rssi);
        lv_obj_set_style_text_color(rs,
            lv_color_hex(s_aps[i].rssi > -60 ? 0x4ADE80 :
                         s_aps[i].rssi > -80 ? 0xFACC15 : 0xF87171), 0);
        lv_obj_align(rs, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    s_built_n = s_n;
    if (s_sel < s_n) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
}

/* ---------- атака deauth ---------- */

static volatile bool s_attack_alive = false;

static void attack_task(void *arg)
{
    s_attack_run = true;
    s_attack_alive = true;
    while (s_attack_run) {
        if (s_attacking)
            s_pkts += netcfg_deauth_one(s_atk_client, 6);
        vTaskDelay(pdMS_TO_TICKS(s_attacking ? 20 : 100));
    }
    s_attack_alive = false;
    vTaskDelete(NULL);
}

/* ---------- список клієнтів обраної точки ---------- */

static void clients_highlight(void)
{
    for (int i = 0; i < s_built_n; i++) {
        lv_obj_set_style_bg_color(s_rows[i],
            lv_color_hex(i == s_csel ? 0x2563EB : 0x161D26), 0);
        lv_obj_set_style_text_color(s_nums[i],
            lv_color_hex(i == s_csel ? 0xFFFFFF : 0x5A6672), 0);
    }
    if (s_csel < s_built_n) lv_obj_scroll_to_view(s_rows[s_csel], LV_ANIM_ON);
}

static void clients_build(void)
{
    lv_obj_clean(s_root);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    /* шапка: назва точки + лічильник */
    lv_obj_t *hdr = lv_label_create(s_root);
    const char *ss = (const char *)s_target.ssid;
    lv_label_set_text_fmt(hdr, "%s • клієнтів: %d",
                          ss[0] ? ss : "(прихована)", s_ncl);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x4ADE80), 0);
    lv_obj_set_width(hdr, 210);
    lv_label_set_long_mode(hdr, LV_LABEL_LONG_DOT);

    if (s_ncl == 0) {
        lv_obj_t *e = lv_label_create(s_root);
        lv_label_set_text(e, "Шукаю клієнтів...\nПристрій має бути активним\n(щось качати/гортати).");
        lv_obj_set_style_text_color(e, lv_color_hex(0x8A94A0), 0);
        s_built_n = 0;
        return;
    }

    for (int i = 0; i < s_ncl; i++) {
        lv_obj_t *row = lv_obj_create(s_root);
        lv_obj_set_size(row, 208, 40);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(i == s_csel ? 0x2563EB : 0x161D26), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        s_rows[i] = row;

        lv_obj_t *num = lv_label_create(row);
        lv_label_set_text_fmt(num, "%d", i + 1);
        lv_obj_set_style_text_color(num,
            lv_color_hex(i == s_csel ? 0xFFFFFF : 0x5A6672), 0);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 0, 0);
        s_nums[i] = num;

        lv_obj_t *mc = lv_label_create(row);
        const uint8_t *m = s_cl[i];
        lv_label_set_text_fmt(mc, "%02X:%02X:%02X:%02X:%02X:%02X",
                              m[0], m[1], m[2], m[3], m[4], m[5]);
        lv_obj_set_style_text_color(mc, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(mc, LV_ALIGN_LEFT_MID, 26, 0);
    }
    s_built_n = s_ncl;
    if (s_csel < s_ncl) lv_obj_scroll_to_view(s_rows[s_csel], LV_ANIM_OFF);
}

static void show_clients(void)
{
    s_view = WT_CLIENTS;
    s_target = s_aps[s_sel];
    s_csel = 0;
    s_built_n = -1;
    s_attacking = false;
    s_ncl = 0;
    lv_label_set_text(s_title, "Клієнти");
    netcfg_raw_begin(s_target.bssid, s_target.primary);
    if (!s_attack_run) xTaskCreate(attack_task, "wt_atk", 3072, NULL, 5, NULL);
    clients_build();
}

/* ---------- атака на одного клієнта ---------- */

static void attack_render(void)
{
    lv_obj_clean(s_root);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *nm = lv_label_create(s_root);
    const char *ss = (const char *)s_target.ssid;
    lv_label_set_text(nm, ss[0] ? ss : "(прихована)");
    lv_obj_set_style_text_font(nm, &font_ua_20, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(nm, 210);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);

    const uint8_t *m = s_atk_client;
    lv_obj_t *cm = lv_label_create(s_root);
    lv_label_set_text_fmt(cm, "ціль: %02X:%02X:%02X:%02X:%02X:%02X",
                          m[0], m[1], m[2], m[3], m[4], m[5]);
    lv_obj_set_style_text_color(cm, lv_color_hex(0xC0C8D0), 0);

    lv_obj_t *st = lv_label_create(s_root);
    lv_label_set_text(st, s_attacking ? "● ГЛУШІННЯ АКТИВНЕ" : "○ зупинено");
    lv_obj_set_style_text_color(st,
        lv_color_hex(s_attacking ? 0xF87171 : 0x8A94A0), 0);

    lv_obj_t *pk = lv_label_create(s_root);
    lv_label_set_text_fmt(pk, "Кадрів: %lu", (unsigned long)s_pkts);
    lv_obj_set_style_text_color(pk, lv_color_hex(0xFACC15), 0);

    lv_obj_t *warn = lv_label_create(s_root);
    lv_label_set_text(warn, "\nЛише для власних мереж!");
    lv_obj_set_style_text_color(warn, lv_color_hex(0x5A6672), 0);

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, s_attacking ? "центр — стоп" : "центр — старт • ліва — до списку");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x35C4F0), 0);
}

static void show_attack(void)
{
    s_view = WT_ATTACK;
    memcpy(s_atk_client, s_cl[s_csel], 6);
    s_attacking = true;      /* тап по клієнту одразу починає атаку */
    s_pkts = 0;
    lv_label_set_text(s_title, "Deauth");
    attack_render();
}

static void stop_attack_mode(void)
{
    s_attacking = false;
    netcfg_raw_end();
}

/* ---------- цикл оновлення ---------- */

static void wt_tick(lv_timer_t *t)
{
    if (s_view == WT_SCAN) {
        if (s_scan_done) {
            s_scan_done = false;
            lv_label_set_text_fmt(s_title, "Мережі (%d)", s_n);
            s_built_n = -1;
            list_build();
        }
        return;
    }
    if (s_view == WT_CLIENTS) {
        /* список клієнтів росте у міру «підслуховування» — перебудова при зміні */
        int n = netcfg_client_snapshot(s_cl, WT_CLI_MAX);
        if (n != s_ncl) {
            s_ncl = n;
            if (s_csel >= s_ncl) s_csel = s_ncl ? s_ncl - 1 : 0;
            s_built_n = -1;
            clients_build();
        }
        return;
    }
    if (s_view == WT_ATTACK) {
        static uint32_t last_pkts = 0;
        if (s_pkts != last_pkts) {
            last_pkts = s_pkts;
            attack_render();
        }
    }
}

/* ---------- app ---------- */

static void wt_open(lv_obj_t *scr)
{
    s_view = WT_SCAN;
    s_built_n = -1;
    s_attacking = false;
    s_attack_run = false;
    s_pkts = 0;

    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "Wi-Fi");
    lv_obj_set_style_text_font(s_title, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 6);

    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 224, 200);
    lv_obj_align(s_root, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 2, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, 6, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_ACTIVE);

    s_timer = lv_timer_create(wt_tick, 300, NULL);
    ESP_LOGI(APPTAG, "wt_open");
    start_scan();
}

static void back_to_clients(void)
{
    s_attacking = false;
    s_view = WT_CLIENTS;
    s_csel = 0;
    s_built_n = -1;
    lv_label_set_text(s_title, "Клієнти");
    s_ncl = netcfg_client_snapshot(s_cl, WT_CLI_MAX);
    clients_build();
}

static void wt_close(void)
{
    /* спершу зупиняємо цикл атаки (щоб задача не відіслала ще один deauth
       вже після вимкнення promiscuous), потім повертаємо радіо у STA-режим */
    s_attacking = false;
    s_attack_run = false;
    for (int i = 0; i < 20 && s_attack_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_view == WT_CLIENTS || s_view == WT_ATTACK) stop_attack_mode();
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
}

static void wt_btn(int btn)
{
    if (s_view == WT_SCAN) {
        if (s_scan_busy) return;
        if (btn == BTN_LEFT_ID)  { if (s_sel > 0) s_sel--; list_highlight(); }
        else if (btn == BTN_RIGHT_ID) { if (s_sel < s_n - 1) s_sel++; list_highlight(); }
        else if (s_n > 0) show_clients();
    } else if (s_view == WT_CLIENTS) {
        /* –/+ курсор по клієнтах; центр — атака на обраного */
        if (btn == BTN_LEFT_ID)  { if (s_csel > 0) s_csel--; clients_highlight(); }
        else if (btn == BTN_RIGHT_ID) { if (s_csel < s_ncl - 1) s_csel++; clients_highlight(); }
        else if (s_ncl > 0) show_attack();
    } else { /* WT_ATTACK */
        if (btn == BTN_MID_ID) { s_attacking = !s_attacking; attack_render(); }
        else if (btn == BTN_LEFT_ID) back_to_clients();
        /* права — вільна; утримання центру — вихід у меню */
    }
}

const app_t app_wifitools = {
    .name = "Wi-Fi атака",
    .open = wt_open,
    .close = wt_close,
    .on_btn = wt_btn,
};

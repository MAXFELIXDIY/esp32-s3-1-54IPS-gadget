/*
 * BLE-сканер. При відкритті: Wi-Fi off + BT on (усе у фоновій задачі, UI не
 * зависає — крутиться спінер).
 * Список: нумерація, сортування за RSSI, до 50 пристроїв, пошук макс. 5 с —
 * далі список фіксується (не стрибає).
 * Керування: ліва/права — курсор; одиночний клік — деталі, ще клік —
 * розшифровка байтів; подвійний клік у списку — новий пошук;
 * утримання центру — у головне меню (каркас, Wi-Fi відновлюється).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
#include "ble_core.h"

static const char *APPTAG = "app_ble";

typedef enum { V_LIST, V_DETAIL, V_DECODED } view_t;
static view_t s_view;

static ble_dev_t s_buf[BLE_MAX_DEVS];
static int s_n, s_sel;
static ble_dev_t s_target;

static int64_t s_scan_start;
static bool s_frozen;                 /* пошук завершено, список зафіксовано */

static lv_obj_t *s_title, *s_root, *s_spinner;
static lv_obj_t *s_rows[BLE_MAX_DEVS];
static lv_obj_t *s_nums[BLE_MAX_DEVS];
static int s_built_n = -1;            /* скільки рядків зараз побудовано */
static lv_timer_t *s_timer;
static lv_timer_t *s_click;           /* одноразовий для детекту подвійного кліку */

#define SCAN_MS 5000

static void decoded_render(void);

/* ---------- спінер ---------- */

static void show_spinner(const char *caption)
{
    lv_obj_clean(s_root);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_spinner = lv_spinner_create(s_root);
    lv_obj_set_size(s_spinner, 48, 48);
    lv_obj_set_style_arc_color(s_spinner, lv_color_hex(0x35C4F0), LV_PART_INDICATOR);
    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, caption);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A94A0), 0);
}

static void ble_init_task(void *arg)
{
    netcfg_wifi_stop();   /* блокуюче — поза UI-потоком */
    ble_start();
    vTaskDelete(NULL);
}

/* ---------- список ---------- */

/* лише перефарбувати вибраний/невибраний рядок + плавно проскролити */
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

/* повна побудова списку (лише коли змінився набір пристроїв) */
static void list_build(void)
{
    lv_obj_clean(s_root);
    s_spinner = NULL;
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    for (int i = 0; i < s_n; i++) {
        lv_obj_t *row = lv_obj_create(s_root);
        lv_obj_set_size(row, 208, 44);
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
        const uint8_t *a = s_buf[i].addr;
        if (s_buf[i].name[0]) lv_label_set_text(nm, s_buf[i].name);
        else lv_label_set_text_fmt(nm, "%02X:%02X:%02X:%02X:%02X:%02X",
                                   a[5], a[4], a[3], a[2], a[1], a[0]);
        lv_obj_set_width(nm, 128);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 28, 0);

        lv_obj_t *rs = lv_label_create(row);
        lv_label_set_text_fmt(rs, "%d", s_buf[i].rssi);
        lv_obj_set_style_text_color(rs,
            lv_color_hex(s_buf[i].rssi > -60 ? 0x4ADE80 :
                         s_buf[i].rssi > -80 ? 0xFACC15 : 0xF87171), 0);
        lv_obj_align(rs, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    s_built_n = s_n;
    if (s_sel < s_n) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
}

static void list_tick(lv_timer_t *t)
{
    if (s_view == V_DECODED) {
        static gatt_state_t last = -1;
        gatt_state_t st = ble_gatt_state();
        if (st != last) { last = st; decoded_render(); }
        return;
    }
    if (s_view != V_LIST) return;
    if (!ble_is_ready()) {
        lv_label_set_text_fmt(s_title, "BLE: %s", ble_stage());
        return;
    }
    if (s_frozen) return;                 /* список зафіксовано — не оновлюємо */

    int64_t now = esp_timer_get_time();
    if (s_scan_start == 0) s_scan_start = now;   /* старт 5-с відліку від готовності */
    if ((now - s_scan_start) / 1000 >= SCAN_MS) {
        ble_scan_stop();                  /* час вийшов — фіксуємо список */
        s_frozen = true;
        s_n = ble_dev_snapshot(s_buf, BLE_MAX_DEVS);
        if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
        lv_label_set_text_fmt(s_title, "BLE (%d)", s_n);
        list_build();
        return;
    }
    ble_scan_resume();
    s_n = ble_dev_snapshot(s_buf, BLE_MAX_DEVS);
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
    lv_label_set_text_fmt(s_title, "Пошук... %d", s_n);
    if (s_n != s_built_n) list_build();   /* перебудова лише коли додались нові */
}

static void new_search(void)
{
    ble_dev_clear();
    s_frozen = false;
    s_sel = 0;
    s_scan_start = esp_timer_get_time();
    lv_label_set_text(s_title, "Пошук... 0");
    ble_scan_resume();
    lv_obj_clean(s_root);
    s_built_n = -1;                       /* примусити перебудову */
}

static void go_list(void)
{
    s_view = V_LIST;
    if (s_timer) lv_timer_resume(s_timer);
    s_n = ble_dev_snapshot(s_buf, BLE_MAX_DEVS);
    if (s_sel >= s_n) s_sel = s_n ? s_n - 1 : 0;
    lv_label_set_text_fmt(s_title, s_frozen ? "BLE (%d)" : "Пошук... %d", s_n);
    s_built_n = -1;
    list_build();
}

/* ---------- деталі ---------- */

static void show_detail(void)
{
    s_view = V_DETAIL;
    if (s_timer) lv_timer_pause(s_timer);
    lv_label_set_text(s_title, "Пристрій");
    lv_obj_clean(s_root);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    const uint8_t *a = s_target.addr;
    char line[64];

    lv_obj_t *nm = lv_label_create(s_root);
    lv_label_set_text(nm, s_target.name[0] ? s_target.name : "(без назви)");
    lv_obj_set_style_text_font(nm, &font_ua_20, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(nm, 210);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);

    snprintf(line, sizeof(line), "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             a[5], a[4], a[3], a[2], a[1], a[0]);
    lv_obj_t *mac = lv_label_create(s_root);
    lv_label_set_text(mac, line);
    lv_obj_set_style_text_color(mac, lv_color_hex(0xC0C8D0), 0);

    lv_obj_t *rs = lv_label_create(s_root);
    lv_label_set_text_fmt(rs, "Сигнал: %d dBm", s_target.rssi);
    lv_obj_set_style_text_color(rs, lv_color_hex(0xC0C8D0), 0);

    lv_obj_t *co = lv_label_create(s_root);
    if (s_target.company != 0xFFFF)
        lv_label_set_text_fmt(co, "Виробник: 0x%04X", s_target.company);
    else lv_label_set_text(co, "Виробник: —");
    lv_obj_set_style_text_color(co, lv_color_hex(0xC0C8D0), 0);

    lv_obj_t *ad = lv_label_create(s_root);
    lv_label_set_text_fmt(ad, "Реклама: %d байт", s_target.adv_len);
    lv_obj_set_style_text_color(ad, lv_color_hex(0x5A6672), 0);

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, "\nцентр — зчитати дані");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x35C4F0), 0);
}

/* ---------- розшифровка (GATT) ---------- */

static void decoded_render(void)
{
    lv_obj_clean(s_root);
    gatt_state_t st = ble_gatt_state();
    const char *msg = st == GATT_CONNECTING ? "З'єднання..." :
                      st == GATT_DISCOVER   ? "Пошук сервісів..." :
                      st == GATT_READING    ? "Зчитування..." :
                      st == GATT_FAIL       ? "Не вдалося під'єднатись" : "";
    if (msg[0]) {
        lv_obj_t *m = lv_label_create(s_root);
        lv_label_set_text(m, msg);
        lv_obj_set_style_text_color(m,
            lv_color_hex(st == GATT_FAIL ? 0xF87171 : 0xC0C8D0), 0);
    }
    if (st == GATT_DONE || st == GATT_READING) {
        ble_char_t chars[BLE_MAX_CHARS];
        int n = ble_gatt_chars(chars, BLE_MAX_CHARS);
        char nm[40], vl[64];
        int shown = 0;
        for (int i = 0; i < n; i++) {
            if (!ble_decode_char(&chars[i], nm, sizeof(nm), vl, sizeof(vl))) continue;
            shown++;
            lv_obj_t *row = lv_obj_create(s_root);
            lv_obj_set_width(row, 210);
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 6, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x161D26), 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 6, 0);
            lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
            lv_obj_t *k = lv_label_create(row);
            lv_label_set_text(k, nm);
            lv_obj_set_style_text_color(k, lv_color_hex(0x35C4F0), 0);
            lv_obj_t *vv = lv_label_create(row);
            lv_label_set_text(vv, vl);
            lv_obj_set_style_text_color(vv, lv_color_hex(0xFFFFFF), 0);
        }
        if (st == GATT_DONE && shown == 0) {
            lv_obj_t *e = lv_label_create(s_root);
            lv_label_set_text(e, "Стандартних даних немає\n(немає відомих характеристик)");
            lv_obj_set_style_text_color(e, lv_color_hex(0x8A94A0), 0);
        }
    }
}

static void show_decoded(void)
{
    s_view = V_DECODED;
    lv_label_set_text(s_title, "Розшифровка");
    lv_obj_clean(s_root);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    if (s_timer) lv_timer_resume(s_timer);
    ble_gatt_connect(s_target.addr, s_target.addr_type);
    decoded_render();
}

/* ---------- детект подвійного кліку у списку ---------- */

static void single_click_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    s_click = NULL;
    if (s_n > 0) { s_target = s_buf[s_sel]; show_detail(); }
}

/* ---------- app ---------- */

static void ble_open(lv_obj_t *scr)
{
    s_view = V_LIST;
    s_sel = 0;
    s_frozen = false;
    s_scan_start = esp_timer_get_time();
    s_click = NULL;
    s_built_n = -1;

    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "BLE: запуск");
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

    show_spinner("Вмикаю Bluetooth...");
    ESP_LOGI(APPTAG, "ble_open: UI готовий, фонова ініціалізація");
    xTaskCreate(ble_init_task, "ble_init", 4096, NULL, 4, NULL);

    s_timer = lv_timer_create(list_tick, 400, NULL);
    /* відлік 5 с рахуємо від моменту готовності BLE, а не відкриття */
    s_scan_start = 0;
}

static void ble_close(void)
{
    if (s_click) { lv_timer_delete(s_click); s_click = NULL; }
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    ble_stop();          /* меню потім увімкне Wi-Fi назад */
}

static void ble_btn(int btn)
{
    if (s_view == V_LIST) {
        /* поки BLE ще вмикається — ігноруємо навігацію */
        if (!ble_is_ready()) return;
        if (s_scan_start == 0) s_scan_start = esp_timer_get_time();

        if (btn == BTN_LEFT_ID) {
            if (s_click) { lv_timer_delete(s_click); s_click = NULL; }
            if (s_sel > 0) s_sel--;
            list_highlight();          /* лише підсвітка + скрол, без перемалювання */
        } else if (btn == BTN_RIGHT_ID) {
            if (s_click) { lv_timer_delete(s_click); s_click = NULL; }
            if (s_sel < s_n - 1) s_sel++;
            list_highlight();
        } else {   /* центр: одиночний -> деталі, подвійний -> новий пошук */
            if (s_click) {
                lv_timer_delete(s_click); s_click = NULL;
                new_search();
            } else if (s_n > 0) {
                s_click = lv_timer_create(single_click_cb, 350, NULL);
            }
        }
    } else if (s_view == V_DETAIL) {
        if (btn == BTN_MID_ID) show_decoded();
        else go_list();
    } else { /* V_DECODED */
        if (btn == BTN_MID_ID) { ble_gatt_disconnect(); go_list(); }
    }
}

const app_t app_ble = {
    .name = "BLE сканер",
    .open = ble_open,
    .close = ble_close,
    .on_btn = ble_btn,
};

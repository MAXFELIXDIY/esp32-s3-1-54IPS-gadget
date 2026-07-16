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
#include "esp_attr.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
#include "ble_core.h"

static const char *APPTAG = "app_ble";

typedef enum { V_LIST, V_DETAIL, V_DECODED, V_NOTIF } view_t;
static view_t s_view;

EXT_RAM_BSS_ATTR static ble_dev_t s_buf[BLE_MAX_DEVS];
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

/* біти властивостей GATT-характеристики (стандарт BLE) */
#define PROP_READ     0x02
#define PROP_WRITE    0x08
#define PROP_NOTIFY   0x10
#define PROP_INDICATE 0x20

static void decoded_render(void);
static void notif_render(void);

/* ---------- утиліти байтів ---------- */

/* hex-рядок "AA BB CC" з обмеженням довжини вихідного буфера */
static void hex_str(const uint8_t *b, int len, char *out, int outsz)
{
    int p = 0;
    for (int i = 0; i < len && p + 3 < outsz; i++)
        p += snprintf(out + p, outsz - p, "%02X ", b[i]);
    if (p > 0) out[p - 1] = 0; else out[0] = 0;
}

/* ASCII-подання: друковані символи як є, решта — '.' */
static void ascii_str(const uint8_t *b, int len, char *out, int outsz)
{
    int i = 0;
    for (; i < len && i < outsz - 1; i++)
        out[i] = (b[i] >= 0x20 && b[i] < 0x7F) ? (char)b[i] : '.';
    out[i] = 0;
}

/* назва типу AD-структури (GAP AD Type) */
static const char *ad_type_name(uint8_t t)
{
    switch (t) {
    case 0x01: return "Flags";
    case 0x02: case 0x03: return "UUID16";
    case 0x06: case 0x07: return "UUID128";
    case 0x08: case 0x09: return "Назва";
    case 0x0A: return "TX Power";
    case 0x16: return "Service Data";
    case 0x19: return "Вигляд";
    case 0xFF: return "Mfg Data";
    default:   return "?";
    }
}

/* картка "ключ / значення" у список */
static void kv_card(const char *key, uint32_t key_color, const char *value)
{
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
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, lv_color_hex(key_color), 0);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_width(v, 196);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(v, lv_color_hex(0xFFFFFF), 0);
}

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

/* Захист від гонки виходу: якщо застосунок закрили, поки ble_init_task ще
   ініціалізує NimBLE, треба гарантувати зупинку BLE (інакше радіо лишиться
   ввімкненим, а Wi-Fi — вимкненим). ble_stop() ідемпотентний. */
static volatile bool s_app_open;
static volatile bool s_init_running;

static void ble_init_task(void *arg)
{
    netcfg_wifi_stop();   /* блокуюче — поза UI-потоком */
    ble_start();
    s_init_running = false;                 /* спершу знімаємо прапорець... */
    if (!s_app_open) ble_stop();            /* ...потім перевіряємо — без гонки */
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
    if (s_view == V_NOTIF) {
        static uint32_t last_cnt = 0;
        uint32_t c = ble_gatt_notif_count();
        if (c != last_cnt) { last_cnt = c; notif_render(); }
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

    /* --- сирі байти реклами + розбір AD-структур --- */
    char hx[3 * BLE_ADV_MAX + 4];
    hex_str(s_target.adv, s_target.adv_len, hx, sizeof(hx));
    char cap[24];
    snprintf(cap, sizeof(cap), "Реклама (%d Б)", s_target.adv_len);
    kv_card(cap, 0x35C4F0, hx[0] ? hx : "—");

    /* прохід TLV: [len][type][data...] */
    for (int i = 0; i + 1 < s_target.adv_len; ) {
        int L = s_target.adv[i];
        if (L == 0 || i + 1 + L > s_target.adv_len) break;
        uint8_t type = s_target.adv[i + 1];
        const uint8_t *d = &s_target.adv[i + 2];
        int dl = L - 1;
        char key[40], val[3 * 32 + 8];
        snprintf(key, sizeof(key), "0x%02X %s", type, ad_type_name(type));
        if (type == 0x08 || type == 0x09) ascii_str(d, dl, val, sizeof(val));
        else hex_str(d, dl > 31 ? 31 : dl, val, sizeof(val));
        kv_card(key, 0x8A94A0, val[0] ? val : "—");
        i += 1 + L;
    }

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, "\n–/+ гортати • центр — зчитати сервіси");
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
        char nm[40], vl[176];
        for (int i = 0; i < n; i++) {
            ble_char_t *c = &chars[i];
            /* ключ: людяна назва (якщо відома) або UUID + прапорці властивостей */
            char key[64], props[8] = "";
            int pp = 0;
            if (c->props & PROP_READ)   props[pp++] = 'R';
            if (c->props & PROP_WRITE)  props[pp++] = 'W';
            if (c->props & PROP_NOTIFY) props[pp++] = 'N';
            if (c->props & PROP_INDICATE) props[pp++] = 'I';
            props[pp] = 0;
            if (ble_decode_char(c, nm, sizeof(nm), vl, sizeof(vl))) {
                snprintf(key, sizeof(key), "%s  [%s]", nm, props);
                kv_card(key, 0x4ADE80, vl);
            } else {
                if (c->is128) {
                    const uint8_t *u = c->uuid128;   /* NimBLE: little-endian */
                    snprintf(key, sizeof(key),
                        "%02X%02X%02X%02X-…-%02X%02X  [%s]",
                        u[15], u[14], u[13], u[12], u[1], u[0], props);
                } else {
                    snprintf(key, sizeof(key), "UUID 0x%04X  [%s]", c->uuid16, props);
                }
                /* значення: hex + ASCII */
                if (c->read_ok && c->val_len) {
                    char hx[3 * 40 + 2], as[42];
                    hex_str(c->val, c->val_len, hx, sizeof(hx));
                    ascii_str(c->val, c->val_len, as, sizeof(as));
                    snprintf(vl, sizeof(vl), "%s\n\"%s\"", hx, as);
                    kv_card(key, 0x35C4F0, vl);
                } else {
                    kv_card(key, 0x35C4F0, (c->props & PROP_READ)
                            ? "(порожньо)" : "(не читається)");
                }
            }
        }
        if (st == GATT_DONE) {
            lv_obj_t *h = lv_label_create(s_root);
            lv_label_set_text(h, "\n–/+ гортати • центр — перехопити потік");
            lv_obj_set_style_text_color(h, lv_color_hex(0xFACC15), 0);
        }
    }
}

/* ---------- перехоплення потоку (notify) ---------- */

static void notif_render(void)
{
    lv_obj_clean(s_root);
    ble_notif_t ns[BLE_MAX_NOTIF];
    int n = ble_gatt_notif_snapshot(ns, BLE_MAX_NOTIF);
    if (n == 0) {
        lv_obj_t *m = lv_label_create(s_root);
        lv_label_set_text(m, "Очікую сповіщень від пристрою...\n(пристрій має щось надсилати)");
        lv_obj_set_style_text_color(m, lv_color_hex(0x8A94A0), 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        char key[48], val[176];
        if (ns[i].uuid16) snprintf(key, sizeof(key), "0x%04X  #%d", ns[i].uuid16, n - i);
        else snprintf(key, sizeof(key), "handle %u  #%d", ns[i].handle, n - i);
        char hx[3 * 40 + 2], as[42];
        hex_str(ns[i].val, ns[i].val_len, hx, sizeof(hx));
        ascii_str(ns[i].val, ns[i].val_len, as, sizeof(as));
        snprintf(val, sizeof(val), "%s\n\"%s\"", hx, as);
        kv_card(key, 0xFACC15, val);
    }
}

static void show_notif(void)
{
    s_view = V_NOTIF;
    lv_label_set_text(s_title, "Перехоплення");
    lv_obj_clean(s_root);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    ble_gatt_subscribe_all();
    if (s_timer) lv_timer_resume(s_timer);
    notif_render();
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
    s_app_open = true;
    s_init_running = true;
    xTaskCreate(ble_init_task, "ble_init", 4096, NULL, 4, NULL);

    s_timer = lv_timer_create(list_tick, 400, NULL);
    /* відлік 5 с рахуємо від моменту готовності BLE, а не відкриття */
    s_scan_start = 0;
}

static void ble_close(void)
{
    s_app_open = false;   /* сигналимо ble_init_task, що застосунок закрито */
    if (s_click) { lv_timer_delete(s_click); s_click = NULL; }
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    /* якщо ініціалізація ще триває — зупинку зробить сам ble_init_task
       (перевіряє s_app_open). Інакше зупиняємо тут. ble_stop ідемпотентний. */
    if (!s_init_running) ble_stop();   /* меню потім увімкне Wi-Fi назад */
}

/* прокрутка вмісту: dir<0 — вгору, dir>0 — вниз (обмежено краями) */
static void scroll_step(int dir)
{
    int32_t room = dir > 0 ? lv_obj_get_scroll_bottom(s_root)
                           : lv_obj_get_scroll_top(s_root);
    if (room <= 0) return;
    int32_t d = room < 80 ? room : 80;
    lv_obj_scroll_by(s_root, 0, dir > 0 ? -d : d, LV_ANIM_ON);
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
        /* –/+ гортають вміст; центр — зчитати сервіси; утримання центру — назад */
        if (btn == BTN_LEFT_ID)  scroll_step(-1);
        else if (btn == BTN_RIGHT_ID) scroll_step(1);
        else show_decoded();
    } else if (s_view == V_DECODED) {
        if (btn == BTN_LEFT_ID)  scroll_step(-1);
        else if (btn == BTN_RIGHT_ID) scroll_step(1);
        else if (ble_gatt_state() == GATT_DONE) show_notif();
    } else { /* V_NOTIF */
        if (btn == BTN_LEFT_ID)  scroll_step(-1);
        else if (btn == BTN_RIGHT_ID) scroll_step(1);
        /* центр вільний; утримання центру — назад у меню */
    }
}

const app_t app_ble = {
    .name = "BLE сканер",
    .open = ble_open,
    .close = ble_close,
    .on_btn = ble_btn,
};

/*
 * FOTA — оновлення прошивки через Wi-Fi.
 * Показує поточну версію та актуальну (tag_name останнього GitHub-релізу).
 * Кнопка «Оновити» внизу активна лише коли версії різняться: користувач
 * наводить на неї фокус кнопкою «+», потім клікає центром.
 *
 * Образ качається у неактивний слот (ota_0/ota_1) через esp_https_ota,
 * після успіху перемикається otadata і пристрій перезавантажується.
 * Відкат на попередній слот — якщо нова прошивка не стартує (див. app_main).
 */
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
#include "settings.h"

/* GitHub API: остання версія (tag_name). Репозиторій той самий, що й у
   FOTA_URL_DEFAULT для завантаження образу. */
#define FOTA_VER_URL \
    "https://api.github.com/repos/MAXFELIXDIY/esp32-s3-1-54IPS-gadget/releases/latest"

static const char *TAG = "fota";

/* стан оновлення: 0 idle/інфо, 1 качається, 2 успіх (ребут), 3 помилка */
static volatile int s_state;
static volatile int s_progress;      /* 0..100 */
static char s_err[64];

/* стан перевірки версії: 0 idle, 1 перевірка, 2 готово, 3 помилка */
static volatile int s_chk;
static char s_latest[24];            /* актуальна версія без 'v' */
static char s_notes[512];            /* опис змін (release body) */

/* екран: 0 — інформація, 1 — повноекранний опис релізу */
static int s_view;
/* вибір на екрані інформації: 0 — вікно нотаток, 1 — кнопка «Оновити» */
static int s_sel;

static lv_obj_t *s_root;
static lv_timer_t *s_timer;

static const char *cur_ver(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "?";
}

/* оновлення доступне: Wi-Fi є, перевірка вдала, версії різні */
static bool update_available(void)
{
    return netcfg_is_connected() && s_chk == 2 && s_latest[0] &&
           strcmp(cur_ver(), s_latest) != 0;
}

/* вікно нотаток можна вибрати лише коли є оновлення й непорожній опис */
static bool notes_selectable(void)
{
    return update_available() && s_notes[0];
}

/* Прибрати з тексту символи, яких немає в шрифті (даються порожніми
   прямокутниками): CR та інші контрольні, крім переводу рядка. */
static void sanitize(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c == '\r') continue;                 /* CRLF -> LF */
        if (c == '\t') { *w++ = ' '; continue; } /* табуляція -> пробіл */
        if (c < 0x20) continue;                  /* інші контрольні */
        *w++ = *r;
    }
    *w = 0;
}

/* ---------------- перевірка версії ---------------- */

static char *http_get(const char *url, int max)
{
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    esp_http_client_set_header(c, "User-Agent", "esp32-gadget");
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    char *buf = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) goto out;
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) != 200) goto out;
    buf = heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM);
    if (!buf) goto out;
    int t = 0, n;
    while (t < max && (n = esp_http_client_read(c, buf + t, max - t)) > 0) t += n;
    buf[t] = 0;
    if (!t) { free(buf); buf = NULL; }
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf;
}

static void check_task(void *arg)
{
    char *body = http_get(FOTA_VER_URL, 20000);
    if (!body) { s_chk = 3; vTaskDelete(NULL); }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { s_chk = 3; vTaskDelete(NULL); }
    cJSON *tag = cJSON_GetObjectItem(j, "tag_name");
    cJSON *notes_j = cJSON_GetObjectItem(j, "body");
    if (cJSON_IsString(tag) && tag->valuestring) {
        const char *t = tag->valuestring;
        if (*t == 'v' || *t == 'V') t++;
        strlcpy(s_latest, t, sizeof(s_latest));
        s_notes[0] = 0;
        if (cJSON_IsString(notes_j) && notes_j->valuestring) {
            strlcpy(s_notes, notes_j->valuestring, sizeof(s_notes));
            sanitize(s_notes);
        }
        s_sel = 0;               /* першим вибирається вікно нотаток */
        s_chk = 2;
    } else {
        s_chk = 3;
    }
    cJSON_Delete(j);
    vTaskDelete(NULL);
}

static void start_check(void)
{
    if (s_chk == 1) return;
    if (!netcfg_is_connected()) { s_chk = 3; return; }
    s_chk = 1;
    s_latest[0] = 0;
    xTaskCreate(check_task, "fotachk", 8192, NULL, 4, NULL);
}

/* ---------------- завантаження образу ---------------- */

static void ota_task(void *arg)
{
    esp_http_client_config_t http = {
        .url = settings_fota_url(),
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        /* Редіректи GitHub несуть підписаний URL (~1 КБ) з JWT: він потрапляє
           і в Location-заголовок відповіді (RX), і в рядок запиту GET до
           release-assets (TX). Стандартні 512 Б обох буферів замалі
           («Out of buffer»). Збільшуємо обидва. */
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };

    esp_https_ota_handle_t h = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK || !h) {
        snprintf(s_err, sizeof(s_err), "begin: %s", esp_err_to_name(err));
        s_state = 3;
        vTaskDelete(NULL);
    }

    int total = esp_https_ota_get_image_size(h);
    while (1) {
        err = esp_https_ota_perform(h);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int done = esp_https_ota_get_image_len_read(h);
        s_progress = (total > 0) ? (int)((int64_t)done * 100 / total) : 0;
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        err = esp_https_ota_finish(h);
        if (err == ESP_OK) {
            s_progress = 100;
            s_state = 2;
            vTaskDelay(pdMS_TO_TICKS(1200));
            esp_restart();
        }
        snprintf(s_err, sizeof(s_err), "finish: %s", esp_err_to_name(err));
    } else {
        esp_https_ota_abort(h);
        snprintf(s_err, sizeof(s_err), "perform: %s", esp_err_to_name(err));
    }
    ESP_LOGE(TAG, "OTA fail: %s", s_err);
    s_state = 3;
    vTaskDelete(NULL);
}

static void start_ota(void)
{
    if (s_state == 1 || s_state == 2) return;
    if (settings_fota_url()[0] == 0) {
        strcpy(s_err, "URL не задано");
        s_state = 3;
        return;
    }
    if (!netcfg_is_connected()) {
        strcpy(s_err, "Немає Wi-Fi");
        s_state = 3;
        return;
    }
    s_progress = 0;
    s_state = 1;
    xTaskCreate(ota_task, "ota", 8192, NULL, 5, NULL);
}

/* ---------------- відображення ---------------- */

static void add_row(lv_obj_t *parent, const char *label, const char *value,
                    uint32_t vcol)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 222, 26);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161D26), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, label);
    lv_obj_set_style_text_color(k, lv_color_hex(0x8A94A0), 0);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, lv_color_hex(vcol), 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void render_info(void)
{
    lv_obj_clean(s_root);
    lv_obj_set_layout(s_root, LV_LAYOUT_NONE);   /* ручна розкладка через align */

    lv_obj_t *hdr = lv_label_create(s_root);
    lv_label_set_text(hdr, "Оновлення прошивки");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xE8ECF0), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);

    /* колонка з інформацією */
    lv_obj_t *col = lv_obj_create(s_root);
    lv_obj_set_size(col, 228, 156);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_opa(col, LV_OPA_0, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);

    add_row(col, "Поточна", cur_ver(), 0xFFFFFF);

    /* актуальна версія — залежно від стану перевірки */
    const char *lat;
    uint32_t latc;
    if (!netcfg_is_connected())      { lat = "немає Wi-Fi"; latc = 0xF87171; }
    else if (s_chk == 1)             { lat = "перевірка...";  latc = 0x8A94A0; }
    else if (s_chk == 3)             { lat = "помилка";       latc = 0xF87171; }
    else if (s_chk == 2 && s_latest[0]) { lat = s_latest;     latc = 0x35C4F0; }
    else                             { lat = "—";             latc = 0x8A94A0; }
    add_row(col, "Актуальна", lat, latc);

    bool en = update_available();
    if (!en || !s_notes[0]) s_sel = 1;   /* без нотаток вибирається лише кнопка */

    if (en && s_notes[0]) {
        /* вибиране вікно «Що нового» (s_sel == 0) */
        char title[48];
        snprintf(title, sizeof(title), "Що нового у %s", s_latest);
        lv_obj_t *nt = lv_label_create(col);
        lv_label_set_text(nt, title);
        lv_obj_set_style_text_color(nt, lv_color_hex(0x4ADE80), 0);

        lv_obj_t *box = lv_obj_create(col);
        lv_obj_set_size(box, 222, 78);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x0E141B), 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_pad_all(box, 6, 0);
        lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        if (s_sel == 0) {                        /* вибрано */
            lv_obj_set_style_border_width(box, 2, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0x35C4F0), 0);
        } else {
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_border_color(box, lv_color_hex(0x1E2A38), 0);
        }

        lv_obj_t *txt = lv_label_create(box);
        lv_obj_set_width(txt, 206);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
        lv_label_set_text(txt, s_notes);
        lv_obj_set_style_text_color(txt, lv_color_hex(0xC7CED6), 0);
        lv_obj_set_style_text_font(txt, &font_ua_16, 0);
    } else {
        /* статусний рядок (перенос, щоб довгий текст не обрізався) */
        lv_obj_t *st = lv_label_create(col);
        lv_obj_set_width(st, 222);
        lv_label_set_long_mode(st, LV_LABEL_LONG_WRAP);
        if (netcfg_is_connected() && s_chk == 2) {
            lv_label_set_text(st, "Версія актуальна");
            lv_obj_set_style_text_color(st, lv_color_hex(0x8A94A0), 0);
        } else if (s_chk == 3 && netcfg_is_connected()) {
            lv_label_set_text(st, "Центр — повторити перевірку");
            lv_obj_set_style_text_color(st, lv_color_hex(0x3A4550), 0);
        } else {
            lv_label_set_text(st, "");
        }
    }

    /* кнопка «Оновити» внизу — вибиране (s_sel == 1) */
    bool bsel = en && s_sel == 1;
    lv_obj_t *btn = lv_obj_create(s_root);
    lv_obj_set_size(btn, 220, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_center(bl);

    if (!en) {                                   /* неактивна */
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x161D26), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_label_set_text(bl, s_chk == 1 ? "Перевірка..." : "Оновити");
        lv_obj_set_style_text_color(bl, lv_color_hex(0x5A6672), 0);
    } else if (bsel) {                           /* активна + вибрана */
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x35C4F0), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(bl, "Оновити");
        lv_obj_set_style_text_color(bl, lv_color_hex(0x08131C), 0);
    } else {                                     /* активна, не вибрана */
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1E2A38), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x35C4F0), 0);
        lv_label_set_text(bl, "Оновити");
        lv_obj_set_style_text_color(bl, lv_color_hex(0xE8ECF0), 0);
    }

    /* підказка навігації */
}

/* повноекранний опис релізу з прокруткою (+/- гортають, центр — назад) */
static lv_obj_t *s_notes_scroll;

static void render_notes(void)
{
    lv_obj_clean(s_root);
    lv_obj_set_layout(s_root, LV_LAYOUT_NONE);

    char title[48];
    snprintf(title, sizeof(title), "Що нового у %s", s_latest);
    lv_obj_t *hdr = lv_label_create(s_root);
    lv_label_set_text(hdr, title);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x4ADE80), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);

    /* прокручуваний контейнер */
    lv_obj_t *sc = lv_obj_create(s_root);
    lv_obj_set_size(sc, 228, 178);
    lv_obj_align(sc, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_color(sc, lv_color_hex(0x0E141B), 0);
    lv_obj_set_style_radius(sc, 6, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_pad_all(sc, 8, 0);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_ON);
    /* без «пружного» виходу за межі та інерції — скрол строго в межах тексту */
    lv_obj_clear_flag(sc, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(sc, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    s_notes_scroll = sc;

    lv_obj_t *txt = lv_label_create(sc);
    lv_obj_set_width(txt, 208);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_label_set_text(txt, s_notes[0] ? s_notes : "—");
    lv_obj_set_style_text_color(txt, lv_color_hex(0xE8ECF0), 0);
    lv_obj_set_style_text_font(txt, &font_ua_16, 0);

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, "+/- гортати,  центр — назад");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void render_progress(void)
{
    char buf[32];
    lv_obj_clean(s_root);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_root, 12, 0);

    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, "Завантаження...");
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8ECF0), 0);

    lv_obj_t *bar = lv_bar_create(s_root);
    lv_obj_set_size(bar, 200, 16);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, s_progress, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x161D26), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x35C4F0), LV_PART_INDICATOR);

    lv_obj_t *pct = lv_label_create(s_root);
    snprintf(buf, sizeof(buf), "%d%%", s_progress);
    lv_label_set_text(pct, buf);
    lv_obj_set_style_text_color(pct, lv_color_hex(0x35C4F0), 0);

    lv_obj_t *warn = lv_label_create(s_root);
    lv_label_set_text(warn, "Не вимикайте живлення");
    lv_obj_set_style_text_color(warn, lv_color_hex(0x8A94A0), 0);
}

static void render_result(bool ok)
{
    lv_obj_clean(s_root);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_root, 10, 0);

    lv_obj_t *l = lv_label_create(s_root);
    if (ok) {
        lv_label_set_text(l, "Готово!\nПерезавантаження...");
        lv_obj_set_style_text_color(l, lv_color_hex(0x4ADE80), 0);
    } else {
        lv_label_set_text(l, s_err[0] ? s_err : "Помилка");
        lv_obj_set_style_text_color(l, lv_color_hex(0xF87171), 0);
    }
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);

    if (!ok) {
        lv_obj_t *hint = lv_label_create(s_root);
        lv_label_set_text(hint, "Центр — назад");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    }
}

/* Вимкнути лейаут кореня: progress/result вмикають flex, а екран інформації
   розкладає елементи вручну через lv_obj_align. Якщо лишити flex увімкненим,
   він ігнорує align і шикує все в ряд/колонку (елементи «тікають» за екран). */
static void root_reset_layout(void)
{
    lv_obj_set_layout(s_root, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_row(s_root, 0, 0);
}

static void app_poll(lv_timer_t *t)
{
    static int last = -2, lastp = -1, lastc = -1;
    if (s_view == 1) { last = s_state; lastc = s_chk; return; }  /* опис — статичний */
    if (s_state == 1) {
        if (last != 1 || lastp != s_progress) { render_progress(); lastp = s_progress; }
    } else if (s_state == 2) {
        if (last != 2) render_result(true);
    } else if (s_state == 3) {
        if (last != 3) render_result(false);
    } else { /* s_state == 0 — екран інформації */
        if (last != 0 || lastc != s_chk) { root_reset_layout(); render_info(); }
    }
    last = s_state;
    lastc = s_chk;
}

static void fota_open(lv_obj_t *scr)
{
    s_state = 0;
    s_progress = 0;
    s_err[0] = 0;
    s_chk = 0;
    s_latest[0] = 0;
    s_notes[0] = 0;
    s_view = 0;
    s_sel = 0;
    s_notes_scroll = NULL;
    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 236, 236);
    lv_obj_center(s_root);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    render_info();
    s_timer = lv_timer_create(app_poll, 300, NULL);
    start_check();
}

static void fota_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    /* якщо йде завантаження — задачу не чіпаємо, вона завершиться сама */
}

static void fota_btn(int btn)
{
    if (s_state == 1 || s_state == 2) return;    /* під час оновлення ігноруємо */

    if (s_state == 3) {                          /* екран помилки — назад в інфо */
        if (btn == BTN_MID_ID) { s_state = 0; render_info(); }
        return;
    }

    if (s_view == 1) {                           /* повноекранний опис релізу */
        if (btn == BTN_RIGHT_ID && s_notes_scroll) {        /* + вниз */
            int room = lv_obj_get_scroll_bottom(s_notes_scroll);   /* лишилось донизу */
            if (room > 0) lv_obj_scroll_by(s_notes_scroll, 0,
                                           -(room < 48 ? room : 48), LV_ANIM_ON);
        } else if (btn == BTN_LEFT_ID && s_notes_scroll) {  /* - вгору */
            int room = lv_obj_get_scroll_top(s_notes_scroll);      /* лишилось догори */
            if (room > 0) lv_obj_scroll_by(s_notes_scroll, 0,
                                           (room < 48 ? room : 48), LV_ANIM_ON);
        } else if (btn == BTN_MID_ID) { s_view = 0; render_info(); } /* назад */
        return;
    }

    /* екран інформації: +/- перемикають вибір між вікном нотаток і кнопкою */
    if (btn == BTN_RIGHT_ID) {                   /* «+» — до кнопки */
        if (update_available() && s_notes[0] && s_sel == 0) { s_sel = 1; render_info(); }
    } else if (btn == BTN_LEFT_ID) {             /* «−» — до вікна нотаток */
        if (s_sel == 1) { s_sel = 0; render_info(); }
    } else if (btn == BTN_MID_ID) {
        if (update_available()) {
            if (s_sel == 0 && s_notes[0]) { s_view = 1; render_notes(); }  /* опис */
            else start_ota();                    /* кнопка — старт */
        } else if (s_chk != 1) {
            start_check();                       /* немає оновлення — перевірити ще */
        }
    }
}

const app_t app_fota = {
    .name = "Оновлення (FOTA)",
    .open = fota_open, .close = fota_close, .on_btn = fota_btn,
};

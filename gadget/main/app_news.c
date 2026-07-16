/*
 * Новинна стрічка: RSS «Української правди».
 * Список із курсором (ліва/права — рух, гортання вниз підвантажує +10),
 * центр — відкрити новину й читати повний текст (з <content:encoded>,
 * очищений від HTML). У деталях: ліва/права — прокрутка, центр — назад.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"

#define NEWS_URL "https://www.pravda.com.ua/rss/"
#define MAX_NEWS 40
#define TITLE_LEN 160
#define RSS_MAX (80 * 1024)
#define PAGE 10
#define BODY_MAX 2400

typedef enum { V_LIST, V_ART } view_t;
static view_t s_view;

static char *s_rss;                 /* весь фід у PSRAM */
EXT_RAM_BSS_ATTR static char s_titles[MAX_NEWS][TITLE_LEN];
static const char *s_item[MAX_NEWS]; /* вказівник на початок <item> */
static int s_total;                 /* розібрано новин */
static int s_shown;                 /* показано у списку */
static int s_sel;                   /* курсор */
static volatile int s_state;        /* 0 idle,1 load,2 ok,3 err */

static lv_obj_t *s_root, *s_status;
static lv_obj_t *s_rows[MAX_NEWS];
static lv_timer_t *s_timer;

/* ---------- мережа ---------- */

static char *http_get(const char *url, char *buf, int max)
{
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "Mozilla/5.0 ESP32",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char *res = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) goto out;
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) != 200) goto out;
    int t = 0, n;
    while (t < max && (n = esp_http_client_read(c, buf + t, max - t)) > 0) t += n;
    buf[t] = 0;
    if (t) res = buf;
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return res;
}

static void unescape(char *s)
{
    struct { const char *e; char c; } E[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''},
    };
    char *w = s, *r = s;
    while (*r) {
        if (*r == '&') {
            bool hit = false;
            for (unsigned k = 0; k < sizeof(E) / sizeof(E[0]); k++) {
                int L = strlen(E[k].e);
                if (!strncmp(r, E[k].e, L)) { *w++ = E[k].c; r += L; hit = true; break; }
            }
            if (hit) continue;
            if (!strncmp(r, "&nbsp;", 6)) { *w++ = ' '; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* очистити HTML-фрагмент [start,end) у out: прибрати теги, стиснути пробіли */
static void strip_html(const char *start, const char *end, char *out, int omax)
{
    int o = 0; bool intag = false, sp = false;
    for (const char *p = start; p < end && o < omax - 1; p++) {
        char ch = *p;
        if (ch == '<') { intag = true; continue; }
        if (ch == '>') { intag = false; continue; }
        if (intag) continue;
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            if (!sp && o > 0) { out[o++] = ' '; sp = true; }
        } else { out[o++] = ch; sp = false; }
    }
    out[o] = 0;
    unescape(out);
}

/* витягти заголовок між <title> і </title>, прибрати CDATA */
static void get_title(const char *item, char *out)
{
    const char *ts = strstr(item, "<title>");
    if (!ts) { out[0] = 0; return; }
    ts += 7;
    const char *te = strstr(ts, "</title>");
    if (!te) { out[0] = 0; return; }
    const char *s = ts, *e = te;
    if (!strncmp(s, "<![CDATA[", 9)) {
        s += 9;
        const char *cd = strstr(s, "]]>");
        if (cd && cd < e) e = cd;
    }
    int len = e - s;
    if (len > TITLE_LEN - 1) len = TITLE_LEN - 1;
    memcpy(out, s, len); out[len] = 0;
    unescape(out);
}

static void fetch_task(void *arg)
{
    if (!http_get(NEWS_URL, s_rss, RSS_MAX)) { s_state = 3; vTaskDelete(NULL); }

    s_total = 0;
    const char *p = s_rss;
    while (s_total < MAX_NEWS) {
        const char *it = strstr(p, "<item>");
        if (!it) break;
        s_item[s_total] = it;
        get_title(it, s_titles[s_total]);
        s_total++;
        p = it + 6;
    }
    s_state = s_total > 0 ? 2 : 3;
    vTaskDelete(NULL);
}

/* ---------- список ---------- */

static void list_render(void)
{
    lv_obj_clean(s_root);
    for (int i = 0; i < s_shown && i < s_total; i++) {
        lv_obj_t *row = lv_obj_create(s_root);
        lv_obj_set_width(row, 216);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row,
            lv_color_hex(i == s_sel ? 0x2563EB : 0x161D26), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 7, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        s_rows[i] = row;

        lv_obj_t *num = lv_label_create(row);
        lv_label_set_text_fmt(num, "%d", i + 1);
        lv_obj_set_style_text_color(num,
            lv_color_hex(i == s_sel ? 0xFFFFFF : 0x35C4F0), 0);
        lv_obj_align(num, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, s_titles[i]);
        lv_obj_set_width(t, 180);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(t, lv_color_hex(0xE8ECF0), 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 24, 0);
    }
    if (s_sel < s_shown) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_ON);
    lv_label_set_text_fmt(s_status, "%d/%d", s_sel + 1, s_total);
}

/* ---------- стаття ---------- */

static void show_article(void)
{
    s_view = V_ART;
    lv_obj_clean(s_root);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, s_titles[s_sel]);
    lv_obj_set_width(title, 210);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);

    /* повний текст з <content:encoded> */
    EXT_RAM_BSS_ATTR static char body[BODY_MAX];
    const char *it = s_item[s_sel];
    const char *ce = strstr(it, "<content:encoded>");
    const char *cd = ce ? strstr(ce, "<![CDATA[") : NULL;
    if (cd) {
        cd += 9;
        const char *e = strstr(cd, "]]>");
        if (!e) e = cd + strlen(cd);
        strip_html(cd, e, body, BODY_MAX);
    } else {
        /* запасний варіант — опис */
        const char *ds = strstr(it, "<description>");
        if (ds) {
            ds += 13;
            const char *de = strstr(ds, "</description>");
            if (de) strip_html(ds, de, body, BODY_MAX);
            else strcpy(body, "(немає тексту)");
        } else strcpy(body, "(немає тексту)");
    }

    lv_obj_t *txt = lv_label_create(s_root);
    lv_label_set_text(txt, body);
    lv_obj_set_width(txt, 210);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(txt, lv_color_hex(0xC8D0D8), 0);

    lv_obj_scroll_to_y(s_root, 0, LV_ANIM_OFF);
    lv_label_set_text(s_status, "читання");
}

/* ---------- керування завантаженням ---------- */

static void start_fetch(void)
{
    if (s_state == 1) return;
    if (!netcfg_is_connected()) { s_state = 3; return; }
    s_state = 1;
    s_shown = PAGE; s_sel = 0;
    lv_obj_clean(s_root);
    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, "Завантаження...");
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A94A0), 0);
    lv_obj_center(l);
    xTaskCreate(fetch_task, "news", 8192, NULL, 4, NULL);
}

static void app_poll(lv_timer_t *t)
{
    if (s_state == 2) {
        if (s_shown > s_total) s_shown = s_total;
        list_render();
        s_state = 0;
    } else if (s_state == 3) {
        lv_obj_clean(s_root);
        lv_obj_t *l = lv_label_create(s_root);
        lv_label_set_text(l, netcfg_is_connected()
            ? "Не вдалося завантажити.\nЦентр — повторити"
            : "Немає Wi-Fi.\nПідключіться в меню Wi-Fi");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xF87171), 0);
        lv_obj_center(l);
        s_state = 0;
    }
}

/* ---------- інтерфейс ---------- */

static void news_open(lv_obj_t *scr)
{
    s_view = V_LIST;
    if (!s_rss) s_rss = heap_caps_malloc(RSS_MAX + 1, MALLOC_CAP_SPIRAM);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Новини");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 6);

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x5A6672), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_label_set_text(s_status, "");

    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 232, 200);
    lv_obj_align(s_root, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 2, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, 6, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_ACTIVE);

    s_timer = lv_timer_create(app_poll, 300, NULL);
    if (s_total > 0) { s_shown = PAGE; s_sel = 0; list_render(); } /* кеш */
    else start_fetch();
}

static void news_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
}

static void news_btn(int btn)
{
    if (s_view == V_ART) {
        if (btn == BTN_MID_ID) { s_view = V_LIST; list_render(); }
        else lv_obj_scroll_by(s_root, 0, btn == BTN_RIGHT_ID ? -80 : 80, LV_ANIM_ON);
        return;
    }
    /* V_LIST */
    if (s_total == 0) { if (btn == BTN_MID_ID) start_fetch(); return; }
    if (btn == BTN_LEFT_ID) {
        if (s_sel > 0) s_sel--;
        list_render();
    } else if (btn == BTN_RIGHT_ID) {
        if (s_sel < s_shown - 1) {
            s_sel++;
        } else if (s_shown < s_total) {         /* догортали — вантажимо +10 */
            s_shown += PAGE;
            if (s_shown > s_total) s_shown = s_total;
            s_sel++;
        }
        list_render();
    } else {                                    /* центр — відкрити новину */
        show_article();
    }
}

const app_t app_news = {
    .name = "Новинна стрічка",
    .open = news_open, .close = news_close, .on_btn = news_btn,
};

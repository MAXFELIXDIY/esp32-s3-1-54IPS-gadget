/*
 * Погода: Open-Meteo (без ключа) + локація за IP (ip-api.com).
 * Детально по одному дню: температура, опис, імовірність опадів, хмарність,
 * вітер, вологість. Права (+) — наступний день (до +3), ліва (−) — попередній.
 * Центр — оновити.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"

#define DEF_LAT 50.4501
#define DEF_LON 30.5234
#define NDAYS 4

typedef struct {
    char city[40];
    /* поточні умови (для сьогодні) */
    float cur_temp, cur_feels, cur_wind;
    int cur_hum, cur_cloud, cur_code;
    /* прогноз по днях */
    int   code[NDAYS], pprob[NDAYS];
    float tmax[NDAYS], tmin[NDAYS], psum[NDAYS], wmax[NDAYS];
} wx_t;

static wx_t s_wx;
static volatile int s_state; /* 0 idle,1 load,2 ok,3 err */
static int s_day;            /* 0..NDAYS-1 */
static lv_obj_t *s_root;
static lv_timer_t *s_timer;

static const char *code_str(int c)
{
    if (c == 0) return "Ясно";
    if (c <= 2) return "Мінлива хмарність";
    if (c == 3) return "Хмарно";
    if (c <= 48) return "Туман";
    if (c <= 57) return "Мряка";
    if (c <= 67) return "Дощ";
    if (c <= 77) return "Сніг";
    if (c <= 82) return "Злива";
    if (c <= 86) return "Снігопад";
    return "Гроза";
}

static const char *day_name(int d)
{
    switch (d) {
    case 0: return "Сьогодні";
    case 1: return "Завтра";
    case 2: return "Післязавтра";
    default: return "Через 3 дні";
    }
}

static char *http_get(const char *url, int max)
{
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    char *buf = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) goto out;
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) != 200) goto out;
    buf = heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM);  /* транзитний HTTP-буфер у PSRAM */
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

static double jnum(cJSON *o, const char *k)
{
    cJSON *j = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(j) ? j->valuedouble : 0;
}
static double jarr(cJSON *o, const char *k, int i)
{
    cJSON *a = cJSON_GetObjectItem(o, k);
    cJSON *e = a ? cJSON_GetArrayItem(a, i) : NULL;
    return cJSON_IsNumber(e) ? e->valuedouble : 0;
}

static void fetch_task(void *arg)
{
    float lat = DEF_LAT, lon = DEF_LON;
    strcpy(s_wx.city, "Київ");

    char *geo = http_get("http://ip-api.com/json", 2048);
    if (geo) {
        cJSON *j = cJSON_Parse(geo);
        if (j) {
            cJSON *a = cJSON_GetObjectItem(j, "lat");
            cJSON *o = cJSON_GetObjectItem(j, "lon");
            cJSON *ci = cJSON_GetObjectItem(j, "city");
            if (cJSON_IsNumber(a) && cJSON_IsNumber(o)) { lat = a->valuedouble; lon = o->valuedouble; }
            if (cJSON_IsString(ci)) strlcpy(s_wx.city, ci->valuestring, sizeof(s_wx.city));
            cJSON_Delete(j);
        }
        free(geo);
    }

    char url[420];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
        "weather_code,wind_speed_10m,cloud_cover"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
        "precipitation_probability_max,precipitation_sum,wind_speed_10m_max"
        "&timezone=auto&forecast_days=4", lat, lon);
    char *body = http_get(url, 8192);
    if (!body) { s_state = 3; vTaskDelete(NULL); }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { s_state = 3; vTaskDelete(NULL); }

    cJSON *cur = cJSON_GetObjectItem(j, "current");
    cJSON *daily = cJSON_GetObjectItem(j, "daily");
    if (cur && daily) {
        s_wx.cur_temp = jnum(cur, "temperature_2m");
        s_wx.cur_feels = jnum(cur, "apparent_temperature");
        s_wx.cur_hum = (int)jnum(cur, "relative_humidity_2m");
        s_wx.cur_wind = jnum(cur, "wind_speed_10m");
        s_wx.cur_cloud = (int)jnum(cur, "cloud_cover");
        s_wx.cur_code = (int)jnum(cur, "weather_code");
        for (int i = 0; i < NDAYS; i++) {
            s_wx.code[i] = (int)jarr(daily, "weather_code", i);
            s_wx.tmax[i] = jarr(daily, "temperature_2m_max", i);
            s_wx.tmin[i] = jarr(daily, "temperature_2m_min", i);
            s_wx.pprob[i] = (int)jarr(daily, "precipitation_probability_max", i);
            s_wx.psum[i] = jarr(daily, "precipitation_sum", i);
            s_wx.wmax[i] = jarr(daily, "wind_speed_10m_max", i);
        }
        s_state = 2;
    } else s_state = 3;
    cJSON_Delete(j);
    vTaskDelete(NULL);
}

static void start_fetch(void)
{
    if (s_state == 1) return;
    if (!netcfg_is_connected()) { s_state = 3; return; }
    s_state = 1;
    lv_obj_clean(s_root);
    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, "Завантаження...");
    lv_obj_set_style_text_color(l, lv_color_hex(0x8A94A0), 0);
    lv_obj_center(l);
    xTaskCreate(fetch_task, "wx", 8192, NULL, 4, NULL);
}

/* примітиви для іконок погоди */
static lv_obj_t *dot(lv_obj_t *p, int d, uint32_t col, int x, int y)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    return o;
}
static void rrect(lv_obj_t *p, int w, int h, int r, uint32_t col, int x, int y)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}

#define CLOUD 0xC7CED6
static void draw_cloud(lv_obj_t *ic, int yoff)
{
    dot(ic, 18, CLOUD, 4, 12 + yoff);
    dot(ic, 24, CLOUD, 12, 6 + yoff);
    dot(ic, 18, CLOUD, 24, 12 + yoff);
    rrect(ic, 40, 12, 6, CLOUD, 4, 20 + yoff);
}

/* контейнер 46x44 з іконкою за кодом погоди */
static lv_obj_t *make_wx_icon(lv_obj_t *parent, int code)
{
    lv_obj_t *ic = lv_obj_create(parent);
    lv_obj_set_size(ic, 46, 44);
    lv_obj_set_style_bg_opa(ic, LV_OPA_0, 0);
    lv_obj_set_style_border_width(ic, 0, 0);
    lv_obj_set_style_pad_all(ic, 0, 0);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ic, LV_SCROLLBAR_MODE_OFF);

    if (code == 0) {                         /* ясно — сонце */
        dot(ic, 30, 0xFFD43B, 8, 7);
    } else if (code <= 2) {                   /* мінлива — сонце + хмара */
        dot(ic, 20, 0xFFD43B, 2, 2);
        draw_cloud(ic, 6);
    } else if (code <= 48) {                  /* хмарно / туман */
        draw_cloud(ic, 4);
        if (code >= 45) {                     /* туман — лінії */
            rrect(ic, 36, 4, 2, 0x8A94A0, 6, 34);
            rrect(ic, 30, 4, 2, 0x8A94A0, 9, 40);
        }
    } else if (code <= 67 || (code >= 80 && code <= 82)) { /* дощ */
        draw_cloud(ic, 0);
        rrect(ic, 3, 10, 1, 0x35C4F0, 12, 32);
        rrect(ic, 3, 10, 1, 0x35C4F0, 22, 34);
        rrect(ic, 3, 10, 1, 0x35C4F0, 32, 32);
    } else if (code <= 77 || code == 85 || code == 86) {   /* сніг */
        draw_cloud(ic, 0);
        dot(ic, 5, 0xFFFFFF, 12, 34);
        dot(ic, 5, 0xFFFFFF, 22, 37);
        dot(ic, 5, 0xFFFFFF, 32, 34);
    } else {                                  /* гроза */
        draw_cloud(ic, 0);
        rrect(ic, 5, 16, 1, 0xFACC15, 20, 30);
    }
    return ic;
}

static void add_row(const char *label, const char *value)
{
    lv_obj_t *row = lv_obj_create(s_root);
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
    lv_obj_set_style_text_color(v, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void render(void)
{
    char buf[64];
    int d = s_day;
    lv_obj_clean(s_root);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, 5, 0);

    /* заголовок: місто + день */
    lv_obj_t *hdr = lv_label_create(s_root);
    snprintf(buf, sizeof(buf), "%s - %s", s_wx.city, day_name(d));
    lv_label_set_text(hdr, buf);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x8A94A0), 0);

    /* велика температура + іконка погоди праворуч */
    int code = (d == 0 ? s_wx.cur_code : s_wx.code[d]);
    lv_obj_t *trow = lv_obj_create(s_root);
    lv_obj_set_size(trow, 230, 46);
    lv_obj_set_style_bg_opa(trow, LV_OPA_0, 0);
    lv_obj_set_style_border_width(trow, 0, 0);
    lv_obj_set_style_pad_all(trow, 0, 0);
    lv_obj_set_scrollbar_mode(trow, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(trow, 10, 0);

    lv_obj_t *temp = lv_label_create(trow);
    if (d == 0) snprintf(buf, sizeof(buf), "%+.0f°C", s_wx.cur_temp);
    else snprintf(buf, sizeof(buf), "%+.0f°C", s_wx.tmax[d]);
    lv_label_set_text(temp, buf);
    lv_obj_set_style_text_font(temp, &font_num_34, 0);
    lv_obj_set_style_text_color(temp, lv_color_hex(0x35C4F0), 0);
    make_wx_icon(trow, code);

    /* опис */
    lv_obj_t *desc = lv_label_create(s_root);
    lv_label_set_text(desc, code_str(d == 0 ? s_wx.cur_code : s_wx.code[d]));
    lv_obj_set_style_text_color(desc, lv_color_hex(0xE8ECF0), 0);

    /* деталі рядками */
    snprintf(buf, sizeof(buf), "%+.0f° / %+.0f°", s_wx.tmax[d], s_wx.tmin[d]);
    add_row("Макс / мін", buf);

    snprintf(buf, sizeof(buf), "%d%%", s_wx.pprob[d]);
    add_row("Імовірність опадів", buf);

    if (d == 0) {
        snprintf(buf, sizeof(buf), "%d%%", s_wx.cur_cloud);
        add_row("Хмарність", buf);
        snprintf(buf, sizeof(buf), "%d%%", s_wx.cur_hum);
        add_row("Вологість", buf);
        snprintf(buf, sizeof(buf), "%+.0f°", s_wx.cur_feels);
        add_row("Відчувається", buf);
        snprintf(buf, sizeof(buf), "%.0f км/год", s_wx.cur_wind);
        add_row("Вітер", buf);
    } else {
        snprintf(buf, sizeof(buf), "%.1f мм", s_wx.psum[d]);
        add_row("Опади за день", buf);
        snprintf(buf, sizeof(buf), "%.0f км/год", s_wx.wmax[d]);
        add_row("Вітер (макс)", buf);
    }

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, d == 0 ? "+ наступний день" :
                            (d == NDAYS - 1 ? "- попередній день" :
                             "- назад   + вперед"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
}

static void app_poll(lv_timer_t *t)
{
    if (s_state == 2) { s_day = 0; render(); s_state = 0; }
    else if (s_state == 3) {
        lv_obj_clean(s_root);
        lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *l = lv_label_create(s_root);
        lv_label_set_text(l, netcfg_is_connected()
            ? "Помилка.\nЦентр — повторити"
            : "Немає Wi-Fi.\nПідключіться в Налаштуваннях");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xF87171), 0);
        lv_obj_center(l);
        s_state = 0;
    }
}

static void wx_open(lv_obj_t *scr)
{
    s_day = 0;
    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 236, 236);
    lv_obj_center(s_root);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, 5, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    s_timer = lv_timer_create(app_poll, 300, NULL);
    start_fetch();
}

static void wx_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
}

static void wx_btn(int btn)
{
    if (s_state != 0) return;         /* поки вантажиться — ігноруємо */
    if (btn == BTN_MID_ID) { start_fetch(); return; }
    /* потрібні готові дані для навігації днями */
    if (btn == BTN_RIGHT_ID && s_day < NDAYS - 1) { s_day++; render(); }
    else if (btn == BTN_LEFT_ID && s_day > 0) { s_day--; render(); }
}

const app_t app_weather = {
    .name = "Прогноз погоди",
    .open = wx_open, .close = wx_close, .on_btn = wx_btn,
};

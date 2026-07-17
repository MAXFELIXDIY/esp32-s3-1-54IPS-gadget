/*
 * FOTA — оновлення прошивки через Wi-Fi.
 * Показує активний OTA-слот і версію збірки. Центр — почати оновлення:
 * образ качається у неактивний слот (ota_0/ota_1) через esp_https_ota,
 * після успіху перемикається otadata і пристрій перезавантажується.
 *
 * URL образу задається у FOTA_URL (порожній = кнопка неактивна).
 * Для наступних кроків URL можна буде зберігати в NVS / вводити з телефона.
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
#include "esp_log.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
#include "settings.h"

static const char *TAG = "fota";

/* стани: 0 idle, 1 качається, 2 успіх (перезавантаження), 3 помилка */
static volatile int s_state;
static volatile int s_progress;   /* 0..100 */
static char s_err[64];

static lv_obj_t *s_root;
static lv_timer_t *s_timer;

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

    int total = esp_https_ota_get_image_size(h);   /* може бути -1, якщо невідомо */
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

static void add_row(const char *label, const char *value, uint32_t vcol)
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
    lv_obj_set_style_text_color(v, lv_color_hex(vcol), 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void render_info(void)
{
    lv_obj_clean(s_root);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, 6, 0);

    lv_obj_t *hdr = lv_label_create(s_root);
    lv_label_set_text(hdr, "Оновлення прошивки");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xE8ECF0), 0);

    const esp_partition_t *run = esp_ota_get_running_partition();
    add_row("Активний слот", run ? run->label : "?", 0xFFFFFF);

    const esp_app_desc_t *desc = esp_app_get_description();
    add_row("Версія", desc ? desc->version : "?", 0x35C4F0);
    add_row("Зібрано", desc ? desc->date : "?", 0xFFFFFF);

    add_row("Wi-Fi", netcfg_is_connected() ? netcfg_ssid() : "немає",
            netcfg_is_connected() ? 0x4ADE80 : 0xF87171);

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    add_row("Відкат при збої", "увімкнено", 0x4ADE80);
#endif

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, settings_fota_url()[0] ? "Центр — оновити" :
                            "URL не задано (settings.c)");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
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
        lv_label_set_text(hint, "Центр — повторити");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    }
}

static void app_poll(lv_timer_t *t)
{
    static int last = -1, lastp = -1;
    if (s_state == 1) {
        if (last != 1 || lastp != s_progress) { render_progress(); lastp = s_progress; }
    } else if (s_state == 2) {
        if (last != 2) render_result(true);
    } else if (s_state == 3) {
        if (last != 3) { render_result(false); s_state = 0; }
    }
    last = s_state;
}

static void fota_open(lv_obj_t *scr)
{
    s_state = 0;
    s_progress = 0;
    s_err[0] = 0;
    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 236, 236);
    lv_obj_center(s_root);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);
    render_info();
    s_timer = lv_timer_create(app_poll, 300, NULL);
}

static void fota_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    /* якщо йде завантаження — задачу не чіпаємо, вона завершиться сама */
}

static void fota_btn(int btn)
{
    if (btn != BTN_MID_ID) return;
    if (s_state == 1 || s_state == 2) return;   /* під час оновлення ігноруємо */
    start_ota();
}

const app_t app_fota = {
    .name = "Оновлення (FOTA)",
    .open = fota_open, .close = fota_close, .on_btn = fota_btn,
};

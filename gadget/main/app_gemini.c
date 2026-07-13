/*
 * Голосовий асистент на Groq (безкоштовно):
 *   мікрофон (I2S RX 16 кГц) -> WAV -> Whisper (розпізнавання) ->
 *   Llama 3.3 (відповідь) -> текст на екрані.
 *
 * Керування у відповіді: -/+ прокрутка (затиск = швидко),
 *   клік центру — озвучити (Google TTS через динамік),
 *   утримання центру — вихід (обробляє каркас).
 * Активація: пункт меню або утримання центру 5 с на головному екрані.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
#include "audio.h"
#include "led.h"
#include "groq_key.h"        /* ключ Groq (gsk_...) */

static const char *TAG = "groq";

#define MIC_WS 4
#define MIC_SCK 5
#define MIC_DIN 6
#define SR 16000
#define REC_SEC 5
#define N_SAMP (SR * REC_SEC)

#define STT_URL  "https://api.groq.com/openai/v1/audio/transcriptions"
#define CHAT_URL "https://api.groq.com/openai/v1/chat/completions"
#define STT_MODEL  "whisper-large-v3-turbo"
#define CHAT_MODEL "llama-3.3-70b-versatile"
#define SYS_PROMPT "Ти дружній голосовий помічник. Відповідай стисло й " \
    "зрозуміло українською, звичайним текстом без розмітки, 2-4 речення."

typedef enum { G_RECORD, G_STT, G_CHAT, G_ANSWER, G_ERR } gstate_t;
static volatile gstate_t s_state;
static char s_question[400];
static char s_answer[3000];

static lv_obj_t *s_scr, *s_root, *s_hint;
static lv_timer_t *s_timer;

/* ---------------- мікрофон -> WAV ---------------- */

static uint8_t *record_wav(size_t *out_len)
{
    i2s_chan_handle_t rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) return NULL;
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SR),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = MIC_SCK, .ws = MIC_WS,
                      .dout = I2S_GPIO_UNUSED, .din = MIC_DIN },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    i2s_channel_init_std_mode(rx, &sc);
    i2s_channel_enable(rx);

    size_t pcm_bytes = N_SAMP * 2, total = 44 + pcm_bytes;
    uint8_t *wav = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    int32_t *blk = malloc(1024 * sizeof(int32_t));
    if (!wav || !blk) { free(blk); free(wav); i2s_del_channel(rx); return NULL; }

    int16_t *pcm = (int16_t *)(wav + 44);
    int got = 0;
    while (got < N_SAMP) {
        size_t br = 0;
        i2s_channel_read(rx, blk, 1024 * sizeof(int32_t), &br, portMAX_DELAY);
        int n = br / sizeof(int32_t);
        for (int i = 0; i < n && got < N_SAMP; i++) {
            int32_t v = blk[i] >> 12;
            if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
            pcm[got++] = (int16_t)v;
        }
    }
    free(blk);
    i2s_channel_disable(rx);
    i2s_del_channel(rx);

    uint32_t brate = SR * 2, dsz = pcm_bytes, rsz = 36 + dsz, sr = SR, f16 = 16;
    uint16_t pcm1 = 1, ch1 = 1, ba = 2, bps = 16;
    memcpy(wav, "RIFF", 4); memcpy(wav + 4, &rsz, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    memcpy(wav + 16, &f16, 4); memcpy(wav + 20, &pcm1, 2);
    memcpy(wav + 22, &ch1, 2); memcpy(wav + 24, &sr, 4);
    memcpy(wav + 28, &brate, 4); memcpy(wav + 32, &ba, 2);
    memcpy(wav + 34, &bps, 2); memcpy(wav + 36, "data", 4);
    memcpy(wav + 40, &dsz, 4);

    *out_len = total;
    return wav;
}

/* ---------------- HTTP ---------------- */

static char *http_read(esp_http_client_handle_t c, int max, int *status)
{
    esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    char *buf = heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;
    int t = 0, n;
    while (t < max && (n = esp_http_client_read(c, buf + t, max - t)) > 0) t += n;
    buf[t] = 0;
    return buf;
}

static void json_escape(const char *in, char *out, int outsz)
{
    int o = 0;
    for (const char *p = in; *p && o < outsz - 2; p++) {
        unsigned char ch = *p;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = ch; }
        else if (ch == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (ch == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (ch == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (ch >= 0x20) out[o++] = ch;
    }
    out[o] = 0;
}

/* Whisper: WAV -> текст (multipart/form-data) */
static bool groq_transcribe(uint8_t *wav, size_t wl, char *out, int outsz)
{
    const char *B = "----ESP32FormBoundaryZ7";
    char pre[512];
    int pl = snprintf(pre, sizeof(pre),
        "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n"
        STT_MODEL "\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nuk\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"a.wav\"\r\nContent-Type: audio/wav\r\n\r\n", B, B, B);
    char suf[64];
    int sl = snprintf(suf, sizeof(suf), "\r\n--%s--\r\n", B);
    int total = pl + (int)wl + sl;

    esp_http_client_config_t cfg = {
        .url = STT_URL, .method = HTTP_METHOD_POST, .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char ct[96];
    snprintf(ct, sizeof(ct), "multipart/form-data; boundary=%s", B);
    esp_http_client_set_header(c, "Content-Type", ct);
    esp_http_client_set_header(c, "Authorization", "Bearer " GROQ_KEY);

    bool ok = false;
    if (esp_http_client_open(c, total) == ESP_OK) {
        esp_http_client_write(c, pre, pl);
        int off = 0;
        while (off < (int)wl) {
            int chunk = (wl - off > 4096) ? 4096 : (wl - off);
            int w = esp_http_client_write(c, (char *)wav + off, chunk);
            if (w <= 0) break;
            off += w;
        }
        esp_http_client_write(c, suf, sl);
        int st = 0;
        char *r = http_read(c, 4096, &st);
        if (r) {
            ESP_LOGI(TAG, "STT HTTP %d", st);
            cJSON *j = cJSON_Parse(r);
            if (j) {
                cJSON *tx = cJSON_GetObjectItem(j, "text");
                if (cJSON_IsString(tx)) {
                    strlcpy(out, tx->valuestring, outsz);
                    /* прибрати провідні пробіли */
                    ok = out[0] != 0;
                }
                cJSON_Delete(j);
            }
            free(r);
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

/* Llama: текст запиту -> відповідь */
static bool groq_chat(const char *q, char *out, int outsz)
{
    char eq[600];
    json_escape(q, eq, sizeof(eq));
    char *body = heap_caps_malloc(1200, MALLOC_CAP_SPIRAM);
    if (!body) return false;
    int bl = snprintf(body, 1200,
        "{\"model\":\"" CHAT_MODEL "\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"" SYS_PROMPT "\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}],\"max_tokens\":600}", eq);

    esp_http_client_config_t cfg = {
        .url = CHAT_URL, .method = HTTP_METHOD_POST, .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", "Bearer " GROQ_KEY);

    bool ok = false;
    if (esp_http_client_open(c, bl) == ESP_OK) {
        if (esp_http_client_write(c, body, bl) == bl) {
            int st = 0;
            char *r = http_read(c, 8192, &st);
            if (r) {
                ESP_LOGI(TAG, "CHAT HTTP %d", st);
                cJSON *j = cJSON_Parse(r);
                if (j) {
                    cJSON *ch = cJSON_GetObjectItem(j, "choices");
                    cJSON *c0 = ch ? cJSON_GetArrayItem(ch, 0) : NULL;
                    cJSON *msg = c0 ? cJSON_GetObjectItem(c0, "message") : NULL;
                    cJSON *ct = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
                    if (cJSON_IsString(ct)) { strlcpy(out, ct->valuestring, outsz); ok = true; }
                    else {
                        cJSON *er = cJSON_GetObjectItem(j, "error");
                        cJSON *em = er ? cJSON_GetObjectItem(er, "message") : NULL;
                        snprintf(out, outsz, "Помилка: %s",
                                 cJSON_IsString(em) ? em->valuestring : "невідома");
                    }
                    cJSON_Delete(j);
                }
                free(r);
            }
        }
    }
    esp_http_client_cleanup(c);
    free(body);
    return ok;
}

/* ---------------- фонова задача ---------------- */

static void work_task(void *arg)
{
    size_t wl = 0;
    uint8_t *wav = record_wav(&wl);
    if (!wav) { strcpy(s_answer, "Помилка мікрофона"); s_state = G_ERR; vTaskDelete(NULL); }

    s_state = G_STT;
    bool ok = groq_transcribe(wav, wl, s_question, sizeof(s_question));
    heap_caps_free(wav);
    if (!ok || !s_question[0]) {
        strcpy(s_answer, "Не вдалося розпізнати мовлення. Спробуйте ще.");
        s_state = G_ERR; vTaskDelete(NULL);
    }

    s_state = G_CHAT;
    ok = groq_chat(s_question, s_answer, sizeof(s_answer));
    s_state = ok ? G_ANSWER : G_ERR;
    vTaskDelete(NULL);
}

/* ---------------- TTS ---------------- */

static int utf8_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

static void speak(void)
{
    static char url[760];
    int p = snprintf(url, sizeof(url),
        "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=uk&q=");
    const char *s = s_answer;
    int cnt = 0;
    /* кодуємо ПОцілими UTF-8 символами (ніколи не ріжемо кирилицю навпіл) */
    while (*s && cnt < 120) {
        int cl = utf8_len((unsigned char)s[0]);
        for (int k = 0; k < cl; k++) if (!s[k]) { cl = k; break; }
        if (cl == 0) break;
        if (p + cl * 3 + 8 >= (int)sizeof(url)) break;   /* лишаємо запас */
        for (int k = 0; k < cl; k++) {
            unsigned char ch = (unsigned char)s[k];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9')) url[p++] = ch;
            else p += snprintf(url + p, sizeof(url) - p, "%%%02X", ch);
        }
        s += cl;
        cnt++;
    }
    url[p] = 0;
    ESP_LOGI(TAG, "TTS URL (%d): %s", p, url);
    audio_play_clip(url);   /* повне завантаження перед відтворенням */
}

/* ---------------- UI ---------------- */

static void show_center(const char *msg, uint32_t col)
{
    lv_obj_clean(s_root);
    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, msg);
    lv_obj_set_width(l, 210);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_center(l);
}

static void show_answer(void)
{
    lv_obj_clean(s_root);
    lv_obj_t *q = lv_label_create(s_root);
    lv_label_set_text_fmt(q, "Ви: %s", s_question);
    lv_obj_set_width(q, 212);
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(q, lv_color_hex(0x5A6672), 0);

    lv_obj_t *t = lv_label_create(s_root);
    lv_label_set_text(t, s_answer);
    lv_obj_set_width(t, 212);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(t, lv_color_hex(0xE8ECF0), 0);

    lv_obj_scroll_to_y(s_root, 0, LV_ANIM_OFF);
    lv_label_set_text(s_hint, "центр — озвучити   2x — новий запит");
}

static void gem_poll(lv_timer_t *tm)
{
    static gstate_t last = -1;
    if (s_state == last) return;
    last = s_state;
    switch (s_state) {
    case G_RECORD: show_center("Говоріть...", 0x35C4F0);
        lv_label_set_text(s_hint, "запис 5 с");
        led_set(60, 40, 0); break;              /* жовтий — слухаю */
    case G_STT: show_center("Розпізнавання...", 0xFACC15);
        lv_label_set_text(s_hint, "");
        led_set(40, 40, 40); break;             /* білий — обробка */
    case G_CHAT: show_center("Думаю...", 0xFACC15);
        lv_label_set_text(s_hint, "");
        led_set(0, 20, 50); break;              /* синій — думаю */
    case G_ANSWER: show_answer();
        led_set(0, 60, 0); break;               /* зелений — готово */
    case G_ERR: show_center(s_answer[0] ? s_answer : "Помилка", 0xF87171);
        lv_label_set_text(s_hint, "центр — спробувати ще");
        led_set(60, 0, 0); break;               /* червоний — помилка */
    }
}

static void start_record(void)
{
    if (!netcfg_is_connected()) {
        s_state = G_ERR;
        strcpy(s_answer, "Немає Wi-Fi.\nПідключіться в Налаштуваннях");
        return;
    }
    s_state = G_RECORD;
    xTaskCreate(work_task, "groq", 8192, NULL, 4, NULL);
}

static void gem_open(lv_obj_t *scr)
{
    s_scr = scr;
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Асистент");
    lv_obj_set_style_text_font(title, &font_ua_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 228, 176);
    lv_obj_align(s_root, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_root, 8, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_ACTIVE);

    s_hint = lv_label_create(scr);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -6);

    s_timer = lv_timer_create(gem_poll, 250, NULL);
    start_record();
}

static void gem_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    audio_stop();
    led_off();
}

static void gem_btn(int btn)
{
    static int64_t last_mid;
    if (s_state == G_ANSWER) {
        if (btn == BTN_MID_ID) {
            int64_t now = esp_timer_get_time();
            if (now - last_mid < 500000) {   /* подвійний клік — новий запит */
                last_mid = 0;
                audio_stop();
                start_record();
            } else {                          /* одиночний — озвучити */
                last_mid = now;
                speak();
            }
        } else {
            lv_obj_scroll_by(s_root, 0, btn == BTN_RIGHT_ID ? -60 : 60, LV_ANIM_ON);
        }
    } else if (s_state == G_ERR) {
        if (btn == BTN_MID_ID) start_record();
    }
}

const app_t app_gemini = {
    .name = "Асистент (Groq)",
    .open = gem_open, .close = gem_close, .on_btn = gem_btn,
};

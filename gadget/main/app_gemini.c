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
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lvgl.h"
#include "apps.h"
#include "netcfg.h"
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
    "зрозуміло українською, звичайним текстом без розмітки, 2-4 речення. " \
    "Твої відповіді показуються на маленькому екрані 240x240 і озвучуються " \
    "голосом. Уникай списків, markdown, довгих цифрових послідовностей."

typedef enum { G_RECORD, G_STT, G_CHAT, G_ANSWER, G_ERR } gstate_t;
static volatile gstate_t s_state;
EXT_RAM_BSS_ATTR static char s_question[400];
EXT_RAM_BSS_ATTR static char s_answer[3000];

/* історія розмови: кільцевий буфер на 3 пари (user+assistant) для контексту.
   У PSRAM, щоб не забирати дефіцитну внутрішню RAM (потрібна аудіо/Wi-Fi). */
#define HIST_PAIRS 3
#define HIST_U 400
#define HIST_A 400
EXT_RAM_BSS_ATTR static char s_hist_u[HIST_PAIRS][HIST_U];
EXT_RAM_BSS_ATTR static char s_hist_a[HIST_PAIRS][HIST_A];
static int  s_hist_n;               /* скільки пар збережено (0..HIST_PAIRS) */

static volatile bool s_busy;        /* work_task жива — захист від подвійного запуску */
static volatile bool s_app_open;    /* застосунок відкритий (для скасування work_task) */

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
    /* Перевіряємо init/enable: якщо I2S_NUM_1 зайнятий (радіо) — не зависаємо. */
    if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK) {
        ESP_LOGE(TAG, "i2s init_std_mode fail");
        i2s_del_channel(rx); return NULL;
    }
    if (i2s_channel_enable(rx) != ESP_OK) {
        ESP_LOGE(TAG, "i2s enable fail");
        i2s_del_channel(rx); return NULL;
    }

    size_t pcm_bytes = N_SAMP * 2, total = 44 + pcm_bytes;
    uint8_t *wav = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    int32_t *blk = malloc(1024 * sizeof(int32_t));
    if (!wav || !blk) {
        free(blk); free(wav);
        i2s_channel_disable(rx); i2s_del_channel(rx); return NULL;
    }

    int16_t *pcm = (int16_t *)(wav + 44);
    int got = 0;
    while (got < N_SAMP) {
        size_t br = 0;
        /* таймаут замість portMAX_DELAY: при збої драйвера виходимо, не зависаємо */
        if (i2s_channel_read(rx, blk, 1024 * sizeof(int32_t), &br,
                             pdMS_TO_TICKS(1000)) != ESP_OK)
            break;
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

    if (got == 0) { free(wav); return NULL; }   /* нічого не записано */
    /* якщо запис обірвався таймаутом — зменшуємо WAV до фактично зчитаного */
    pcm_bytes = (size_t)got * 2;
    total = 44 + pcm_bytes;

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
#define CHAT_BODY 8192

static bool groq_chat(const char *q, char *out, int outsz)
{
    char *body = heap_caps_malloc(CHAT_BODY, MALLOC_CAP_SPIRAM);
    char *esc  = heap_caps_malloc(2 * HIST_A + 8, MALLOC_CAP_SPIRAM);
    if (!body || !esc) { free(body); free(esc); return false; }

    int bl = snprintf(body, CHAT_BODY,
        "{\"model\":\"" CHAT_MODEL "\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"" SYS_PROMPT "\"}");
    /* попередні пари для контексту розмови */
    for (int i = 0; i < s_hist_n; i++) {
        json_escape(s_hist_u[i], esc, 2 * HIST_A + 8);
        bl += snprintf(body + bl, CHAT_BODY - bl,
                       ",{\"role\":\"user\",\"content\":\"%s\"}", esc);
        json_escape(s_hist_a[i], esc, 2 * HIST_A + 8);
        bl += snprintf(body + bl, CHAT_BODY - bl,
                       ",{\"role\":\"assistant\",\"content\":\"%s\"}", esc);
    }
    /* поточне питання */
    json_escape(q, esc, 2 * HIST_A + 8);
    bl += snprintf(body + bl, CHAT_BODY - bl,
                   ",{\"role\":\"user\",\"content\":\"%s\"}],\"max_tokens\":600}", esc);
    free(esc);

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

/* Виставити стан після повного запису s_answer/s_question. Барʼєр release
   гарантує, що на іншому ядрі (LVGL-потік читає у gem_poll) буфер уже
   записаний до того, як стане видимим новий s_state. */
static void set_state(gstate_t st)
{
    __sync_synchronize();
    s_state = st;
}

/* Додати пару (питання, відповідь) в історію (кільцевий буфер на HIST_PAIRS). */
static void hist_push(const char *u, const char *a)
{
    if (s_hist_n < HIST_PAIRS) {
        strlcpy(s_hist_u[s_hist_n], u, HIST_U);
        strlcpy(s_hist_a[s_hist_n], a, HIST_A);
        s_hist_n++;
    } else {
        for (int i = 1; i < HIST_PAIRS; i++) {
            memcpy(s_hist_u[i - 1], s_hist_u[i], HIST_U);
            memcpy(s_hist_a[i - 1], s_hist_a[i], HIST_A);
        }
        strlcpy(s_hist_u[HIST_PAIRS - 1], u, HIST_U);
        strlcpy(s_hist_a[HIST_PAIRS - 1], a, HIST_A);
    }
}

static void work_task(void *arg)
{
    size_t wl = 0;
    uint8_t *wav = record_wav(&wl);
    if (!wav) {
        strcpy(s_answer, "Помилка мікрофона");
        set_state(G_ERR); s_busy = false; vTaskDelete(NULL);
    }
    /* застосунок закрили під час запису — не витрачаємо мережу далі */
    if (!s_app_open) { heap_caps_free(wav); s_busy = false; vTaskDelete(NULL); }

    set_state(G_STT);
    bool ok = groq_transcribe(wav, wl, s_question, sizeof(s_question));
    heap_caps_free(wav);
    if (!s_app_open) { s_busy = false; vTaskDelete(NULL); }
    if (!ok || !s_question[0]) {
        strcpy(s_answer, "Не вдалося розпізнати мовлення. Спробуйте ще.");
        set_state(G_ERR); s_busy = false; vTaskDelete(NULL);
    }

    set_state(G_CHAT);
    ok = groq_chat(s_question, s_answer, sizeof(s_answer));
    if (ok) hist_push(s_question, s_answer);   /* контекст для наступних питань */
    set_state(ok ? G_ANSWER : G_ERR);
    s_busy = false;
    vTaskDelete(NULL);
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
    lv_label_set_text(s_hint, "–/+ гортати • утримання + — новий запит");
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
    if (s_busy) return;                 /* попередня задача ще працює */
    if (!netcfg_is_connected()) {
        s_state = G_ERR;
        strcpy(s_answer, "Немає Wi-Fi.\nПідключіться в Налаштуваннях");
        return;
    }
    s_busy = true;
    s_state = G_RECORD;
    if (xTaskCreate(work_task, "groq", 8192, NULL, 4, NULL) != pdPASS)
        s_busy = false;                 /* не створилась — знімаємо прапорець */
}

static void gem_open(lv_obj_t *scr)
{
    s_scr = scr;
    s_app_open = true;
    s_hist_n = 0;               /* нова сесія — чистимо контекст розмови */
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
    s_app_open = false;   /* work_task зупиниться між етапами */
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    led_off();
}

static void gem_btn(int btn)
{
    if (s_state == G_ANSWER) {
        /* –/+ — прокрутка тексту (утримання + = новий запит, див. gem_hold);
           центр вільний */
        if (btn == BTN_LEFT_ID || btn == BTN_RIGHT_ID)
            lv_obj_scroll_by(s_root, 0, btn == BTN_RIGHT_ID ? -60 : 60, LV_ANIM_ON);
    } else if (s_state == G_ERR) {
        if (btn == BTN_MID_ID) start_record();
    }
}

/* утримання бокової кнопки: + (права) — новий запит */
static void gem_hold(int btn)
{
    if (btn == BTN_RIGHT_ID && (s_state == G_ANSWER || s_state == G_ERR))
        start_record();
}

const app_t app_gemini = {
    .name = "Асистент (Groq)",
    .open = gem_open, .close = gem_close, .on_btn = gem_btn, .on_hold = gem_hold,
};

/*
 * Аудіоконвеєр інтернет-радіо:
 *   HTTP-стрім (MP3) -> кільцевий буфер у PSRAM -> Helix MP3 -> I2S.
 *
 * Динамік: I2S DOUT=7, BCLK=15, LRCK=16 (підсилювач без enable-піна).
 * Гучність — програмне масштабування семплів (0..256).
 */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mp3dec.h"
#include "audio.h"

static const char *TAG = "audio";

#define SPK_DOUT 7
#define SPK_BCLK 15
#define SPK_LRCK 16

#define STREAM_BUF_SZ   (160 * 1024)  /* PSRAM */
#define PREBUFFER_BYTES (48 * 1024)
#define INBUF_SZ        (16 * 1024)
#define HTTP_CHUNK      4096

static i2s_chan_handle_t s_tx;
static StreamBufferHandle_t s_sb;
static StaticStreamBuffer_t s_sb_struct;
static uint8_t *s_sb_storage;

static _Atomic int s_session = 0;
static volatile bool s_http_running = false;
static volatile bool s_dec_running = false;
static volatile bool s_eos = false;   /* потік завершився природно (файл) */

static volatile audio_state_t s_state = AUDIO_STOPPED;
static char s_info[64] = "";
static volatile int s_volume = 200;

static char s_url[800];   /* великий: TTS-URL з percent-кодуванням довгий */

/* ---------------- I2S ---------------- */

static int s_cur_rate = 0;
static bool s_i2s_on = false;

static void i2s_setup(void)
{
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK,
            .ws   = SPK_LRCK,
            .dout = SPK_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    s_cur_rate = 44100;
    /* канал вмикаємо лише під час відтворення (audio_play), інакше
       підсилювач без даних гудить — тримаємо його вимкненим = тиша */
    s_i2s_on = false;
}

/* увімкнути/вимкнути I2S: без тактів LRCK підсилювач переходить у режим
   спокою й на паузі стоїть повна тиша (жодного ритмічного шуму) */
static void i2s_on(bool on)
{
    if (on == s_i2s_on) return;
    if (on) i2s_channel_enable(s_tx);
    else i2s_channel_disable(s_tx);
    s_i2s_on = on;
}

static void i2s_set_rate(int rate)
{
    if (rate == s_cur_rate) return;
    ESP_LOGI(TAG, "частота потоку: %d Гц", rate);
    bool was = s_i2s_on;
    if (was) i2s_channel_disable(s_tx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (was) i2s_channel_enable(s_tx);
    s_cur_rate = rate;
}

/* ---------------- HTTP-стрім ---------------- */

static void http_task(void *arg)
{
    int session = (int)(intptr_t)arg;
    s_http_running = true;

    esp_http_client_config_t cfg = {
        .url = s_url,
        .timeout_ms = 8000,
        .buffer_size = HTTP_CHUNK,
        .buffer_size_tx = 2048,   /* довгий TTS-URL не вміщається у 512 */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "Mozilla/5.0 (compatible; ESP32)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) goto fail;

    if (esp_http_client_open(client, 0) != ESP_OK) goto fail_client;
    esp_http_client_fetch_headers(client);

    /* до 3 редіректів вручну (open/read не робить їх автоматично) */
    for (int r = 0; r < 3; r++) {
        int status = esp_http_client_get_status_code(client);
        if (status < 300 || status >= 400) break;
        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (esp_http_client_open(client, 0) != ESP_OK) goto fail_client;
        esp_http_client_fetch_headers(client);
    }
    if (esp_http_client_get_status_code(client) != 200) {
        ESP_LOGE(TAG, "HTTP статус %d",
                 esp_http_client_get_status_code(client));
        goto fail_client;
    }

    uint8_t *buf = heap_caps_malloc(HTTP_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) { s_state = AUDIO_ERROR; goto done_close; }
    while (session == atomic_load(&s_session)) {
        int n = esp_http_client_read(client, (char *)buf, HTTP_CHUNK);
        if (n <= 0) {
            /* природний кінець файлу (напр., TTS-кліп) */
            if (esp_http_client_is_complete_data_received(client)) s_eos = true;
            ESP_LOGW(TAG, "кінець потоку (%d, eos=%d)", n, s_eos);
            break;
        }
        int off = 0;
        while (off < n && session == atomic_load(&s_session)) {
            off += xStreamBufferSend(s_sb, buf + off, n - off,
                                     pdMS_TO_TICKS(200));
        }
    }
    free(buf);
    /* помилка лише якщо обрив без коректного завершення файлу */
    if (session == atomic_load(&s_session) && !s_eos) s_state = AUDIO_ERROR;
done_close:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    s_http_running = false;
    vTaskDelete(NULL);

fail_client:
    esp_http_client_cleanup(client);
fail:
    if (session == atomic_load(&s_session)) s_state = AUDIO_ERROR;
    s_http_running = false;
    vTaskDelete(NULL);
}

/* ---------------- Декодер ---------------- */

static void decode_task(void *arg)
{
    int session = (int)(intptr_t)arg;
    s_dec_running = true;

    HMP3Decoder dec = MP3InitDecoder();
    /* великі буфери у PSRAM: економимо дефіцитну внутрішню RAM (лишаються
       тільки стеки задач). Для 128 кбіт/с пропускної здатності PSRAM удосталь. */
    uint8_t *inbuf = heap_caps_malloc(INBUF_SZ, MALLOC_CAP_SPIRAM);
    /* 1152 семплів на канал максимум */
    int16_t *pcm = heap_caps_malloc(1152 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *out = heap_caps_malloc(1152 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int inlen = 0;
    if (!dec || !inbuf || !pcm || !out) {
        ESP_LOGE(TAG, "decode_task: нестача RAM для буферів");
        if (dec) MP3FreeDecoder(dec);
        free(inbuf); free(pcm); free(out);
        s_state = AUDIO_ERROR;
        s_dec_running = false;
        vTaskDelete(NULL);
    }

    /* попередня буферизація */
    s_state = AUDIO_BUFFERING;
    while (session == atomic_load(&s_session) &&
           xStreamBufferBytesAvailable(s_sb) < PREBUFFER_BYTES &&
           s_state != AUDIO_ERROR && !s_eos) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (session == atomic_load(&s_session) && s_state != AUDIO_ERROR) {
        /* доливаємо вхідний буфер */
        if (inlen < INBUF_SZ) {
            size_t got = xStreamBufferReceive(s_sb, inbuf + inlen,
                                              INBUF_SZ - inlen,
                                              pdMS_TO_TICKS(300));
            if (got == 0 && inlen < 2048) {
                /* кінець файлу і все відтворено — завершуємо чисто */
                if (s_eos && xStreamBufferBytesAvailable(s_sb) == 0) break;
                if (s_state == AUDIO_PLAYING) {
                    ESP_LOGW(TAG, "буфер порожній, чекаю...");
                    i2s_on(false);   /* мют на час ре-буферизації — без повтору DMA */
                    s_state = AUDIO_BUFFERING;
                }
                continue;
            }
            inlen += got;
        }

        int off = MP3FindSyncWord(inbuf, inlen);
        if (off < 0) { inlen = 0; continue; }
        if (off > 0) {
            memmove(inbuf, inbuf + off, inlen - off);
            inlen -= off;
        }

        uint8_t *rp = inbuf;
        int left = inlen;
        int err = MP3Decode(dec, &rp, &left, pcm, 0);
        if (err == ERR_MP3_NONE) {
            int used = inlen - left;
            memmove(inbuf, inbuf + used, left);
            inlen = left;

            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);
            if (fi.samprate > 0) i2s_set_rate(fi.samprate);
            snprintf(s_info, sizeof(s_info), "%d кбіт/с - %d кГц",
                     fi.bitrate / 1000, fi.samprate / 1000);
            i2s_on(true);            /* перший готовий кадр — знімаємо мют */
            s_state = AUDIO_PLAYING;

            /* моно-мікс в обидва I2S-слоти + гучність */
            int frames = (fi.nChans == 2) ? fi.outputSamps / 2
                                          : fi.outputSamps;
            int vol = s_volume;
            for (int i = 0; i < frames; i++) {
                int32_t s;
                if (fi.nChans == 2)
                    s = ((int32_t)pcm[2 * i] + pcm[2 * i + 1]) / 2;
                else
                    s = pcm[i];
                s = (s * vol) >> 8;
                out[2 * i] = out[2 * i + 1] = (int16_t)s;
            }
            size_t written;
            i2s_channel_write(s_tx, out, frames * 2 * sizeof(int16_t),
                              &written, portMAX_DELAY);
        } else if (err == ERR_MP3_INDATA_UNDERFLOW ||
                   err == ERR_MP3_MAINDATA_UNDERFLOW) {
            /* треба більше даних — на початок циклу */
            if (inlen >= INBUF_SZ) inlen = 0; /* захист від клину */
        } else {
            /* биті дані — пропускаємо байт і шукаємо sync далі */
            memmove(inbuf, inbuf + 1, --inlen);
        }
    }

    MP3FreeDecoder(dec);
    free(inbuf); free(pcm); free(out);
    /* чиста зупинка після завершення файлу — без «строба» */
    if (session == atomic_load(&s_session) && s_eos) {
        i2s_on(false);
        s_state = AUDIO_STOPPED;
    }
    s_dec_running = false;
    vTaskDelete(NULL);
}

/* ---------------- API ---------------- */

void audio_init(void)
{
    static bool inited = false;
    if (inited) return;
    inited = true;
    s_sb_storage = heap_caps_malloc(STREAM_BUF_SZ, MALLOC_CAP_SPIRAM);
    assert(s_sb_storage);
    s_sb = xStreamBufferCreateStatic(STREAM_BUF_SZ, 1, s_sb_storage,
                                     &s_sb_struct);
    i2s_setup();
}

void audio_stop(void)
{
    atomic_fetch_add(&s_session, 1);
    for (int i = 0; i < 30 && (s_http_running || s_dec_running); i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    xStreamBufferReset(s_sb);
    i2s_on(false);            /* вимикаємо підсилювач → повна тиша */
    s_state = AUDIO_STOPPED;
    s_info[0] = 0;
}

void audio_play(const char *url)
{
    audio_stop();
    s_eos = false;
    /* I2S НЕ вмикаємо тут: поки йде зʼєднання/буферизація нема свіжих даних,
       і ввімкнений підсилювач повторював би старий вміст DMA («зациклювання»).
       Вмикаємо у decode_task рівно коли готовий перший кадр PCM. */
    strlcpy(s_url, url, sizeof(s_url));
    s_state = AUDIO_CONNECTING;
    int session = atomic_load(&s_session);

    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "старт: вільно internal %u, найбільший блок %u",
             (unsigned)freeb, (unsigned)big);

    /* створення задач може не вдатися при нестачі внутрішньої RAM (стеки
       12+6 КБ): тоді стан завис би на CONNECTING — тому перевіряємо й
       звітуємо ERROR, а не мовчазне зависання */
    TaskHandle_t ht = NULL, dt = NULL;
    if (xTaskCreate(http_task, "radio_http", 4096, (void *)(intptr_t)session, 5,
                    &ht) != pdPASS) {
        ESP_LOGE(TAG, "не вдалося створити radio_http (мало RAM)");
        s_state = AUDIO_ERROR; i2s_on(false);
        return;
    }
    if (xTaskCreate(decode_task, "radio_dec", 8192, (void *)(intptr_t)session, 6,
                    &dt) != pdPASS) {
        ESP_LOGE(TAG, "не вдалося створити radio_dec (мало RAM)");
        /* зупиняємо вже створений http_task через зміну сесії */
        atomic_fetch_add(&s_session, 1);
        s_state = AUDIO_ERROR; i2s_on(false);
        return;
    }
}

audio_state_t audio_state(void) { return s_state; }
const char *audio_info(void) { return s_info; }
void audio_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 256) v = 256;
    s_volume = v;
}
int audio_get_volume(void) { return s_volume; }

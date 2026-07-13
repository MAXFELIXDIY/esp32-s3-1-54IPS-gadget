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

static volatile audio_state_t s_state = AUDIO_STOPPED;
static char s_info[64] = "";
static volatile int s_volume = 200;

static char s_url[256];

/* ---------------- I2S ---------------- */

static int s_cur_rate = 0;

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
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

static void i2s_set_rate(int rate)
{
    if (rate == s_cur_rate) return;
    ESP_LOGI(TAG, "частота потоку: %d Гц", rate);
    i2s_channel_disable(s_tx);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(s_tx, &clk);
    i2s_channel_enable(s_tx);
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
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "ESP32-Radio/1.0",
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

    uint8_t *buf = malloc(HTTP_CHUNK);
    while (session == atomic_load(&s_session)) {
        int n = esp_http_client_read(client, (char *)buf, HTTP_CHUNK);
        if (n <= 0) {
            ESP_LOGW(TAG, "потік обірвався (%d)", n);
            break;
        }
        int off = 0;
        while (off < n && session == atomic_load(&s_session)) {
            off += xStreamBufferSend(s_sb, buf + off, n - off,
                                     pdMS_TO_TICKS(200));
        }
    }
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    /* якщо сесія ще актуальна — потік упав сам */
    if (session == atomic_load(&s_session)) s_state = AUDIO_ERROR;
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
    uint8_t *inbuf = malloc(INBUF_SZ);
    /* 1152 семплів на канал максимум */
    int16_t *pcm = malloc(1152 * 2 * sizeof(int16_t));
    int16_t *out = malloc(1152 * 2 * sizeof(int16_t));
    int inlen = 0;

    /* попередня буферизація */
    s_state = AUDIO_BUFFERING;
    while (session == atomic_load(&s_session) &&
           xStreamBufferBytesAvailable(s_sb) < PREBUFFER_BYTES &&
           s_state != AUDIO_ERROR) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (session == atomic_load(&s_session) && s_state != AUDIO_ERROR) {
        /* доливаємо вхідний буфер */
        if (inlen < INBUF_SZ) {
            size_t got = xStreamBufferReceive(s_sb, inbuf + inlen,
                                              INBUF_SZ - inlen,
                                              pdMS_TO_TICKS(300));
            if (got == 0 && inlen < 2048) {
                if (s_state == AUDIO_PLAYING) {
                    ESP_LOGW(TAG, "буфер порожній, чекаю...");
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
    s_dec_running = false;
    vTaskDelete(NULL);
}

/* ---------------- API ---------------- */

void audio_init(void)
{
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
    s_state = AUDIO_STOPPED;
    s_info[0] = 0;
}

void audio_play(const char *url)
{
    audio_stop();
    strlcpy(s_url, url, sizeof(s_url));
    s_state = AUDIO_CONNECTING;
    int session = atomic_load(&s_session);
    xTaskCreate(http_task, "radio_http", 6144, (void *)(intptr_t)session, 5,
                NULL);
    xTaskCreate(decode_task, "radio_dec", 12288, (void *)(intptr_t)session, 6,
                NULL);
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

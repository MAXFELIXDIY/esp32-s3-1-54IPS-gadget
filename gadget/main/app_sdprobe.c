/*
 * SD-зонд: визначає, на які GPIO розведено microSD-слот.
 *
 * Метод: програмний (bit-bang) SPI. На кожній комбінації 4 вільних пінів
 * (SCK/MOSI/MISO/CS) надсилаємо картці CMD0 (GO_IDLE_STATE) і чекаємо на
 * коректну відповідь 0x01 (картка перейшла в idle у SPI-режимі). Якщо
 * відповіла — комбінація правильна.
 *
 * Кандидати — вільні виведені піни (не зайняті дисплеєм/аудіо/кнопками):
 *   21, 38, 43(TX0), 44(RX0), 45, 11, 12.
 *
 * Керування: центр — старт/повтор; утримання центру — вихід.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "lvgl.h"
#include "apps.h"

static const char *APPTAG = "sdprobe";

/* Усі безпечні для перемикання вільні GPIO. Виключено: flash (26–32),
   октальний PSRAM (33–37), USB (19,20), дисплей (8,9,10,13,14,18),
   аудіо (4,5,6,7,15,16), кнопки (0,39,40). Піни слота SD часто НЕ виведені
   на гребінки — тому перебираємо ширше, а не лише роз'ємні. */
static const int CAND[] = { 1, 2, 3, 11, 12, 17, 21, 38, 41, 42, 43, 44, 45, 47, 48 };
#define NCAND (int)(sizeof(CAND) / sizeof(CAND[0]))

typedef enum { SP_IDLE, SP_RUN, SP_DONE } sp_state_t;
static volatile sp_state_t s_state;
static volatile int s_progress;              /* скільки комбінацій перевірено */
static volatile int s_total;
static volatile bool s_found;
static volatile bool s_abort;                /* перервати перебір (вихід) */
static volatile int s_sck, s_mosi, s_miso, s_cs;   /* результат */

static lv_obj_t *s_root, *s_title;
static lv_timer_t *s_timer;

/* ---------- bit-bang SPI mode 0 ---------- */

static int g_sck, g_mosi, g_miso, g_cs;

static inline void clk_delay(void) { esp_rom_delay_us(3); }   /* ~150 кГц */

static uint8_t spi_xfer(uint8_t out)
{
    uint8_t in = 0;
    for (int b = 7; b >= 0; b--) {
        gpio_set_level(g_mosi, (out >> b) & 1);
        clk_delay();
        gpio_set_level(g_sck, 1);
        in = (in << 1) | (gpio_get_level(g_miso) & 1);
        clk_delay();
        gpio_set_level(g_sck, 0);
    }
    return in;
}

/* надіслати команду SD-SPI, повернути R1; за потреби зчитати 4 байти R3/R7 */
static uint8_t send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *trail)
{
    spi_xfer(0xFF);
    spi_xfer(0x40 | cmd);
    spi_xfer(arg >> 24); spi_xfer(arg >> 16); spi_xfer(arg >> 8); spi_xfer(arg);
    spi_xfer(crc);
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10; i++) { r1 = spi_xfer(0xFF); if (!(r1 & 0x80)) break; }
    if (trail) for (int i = 0; i < 4; i++) trail[i] = spi_xfer(0xFF);
    return r1;
}

/* true лише якщо картка відповіла idle на CMD0 І повернула ехо 0xAA на CMD8 —
   подвійна перевірка виключає випадкові збіги на «плаваючому» MISO */
static bool try_card(int sck, int mosi, int miso, int cs)
{
    g_sck = sck; g_mosi = mosi; g_miso = miso; g_cs = cs;
    gpio_set_direction(sck, GPIO_MODE_OUTPUT);
    gpio_set_direction(mosi, GPIO_MODE_OUTPUT);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    gpio_set_direction(miso, GPIO_MODE_INPUT);
    gpio_set_pull_mode(miso, GPIO_PULLUP_ONLY);
    gpio_set_level(sck, 0);
    gpio_set_level(mosi, 1);
    gpio_set_level(cs, 1);

    for (int i = 0; i < 12; i++) spi_xfer(0xFF);   /* прокинути: ≥74 такти */

    gpio_set_level(cs, 0);
    uint8_t r1 = send_cmd(0, 0, 0x95, NULL);       /* CMD0 → очікуємо 0x01 */
    bool ok = false;
    if (r1 == 0x01) {
        uint8_t t[4];
        uint8_t r = send_cmd(8, 0x000001AA, 0x87, t);  /* CMD8 → R7 з ехо 0xAA */
        ok = (r == 0x01 && t[2] == 0x01 && t[3] == 0xAA);
    }
    gpio_set_level(cs, 1);
    spi_xfer(0xFF);

    gpio_set_direction(sck, GPIO_MODE_INPUT);
    gpio_set_direction(mosi, GPIO_MODE_INPUT);
    gpio_set_direction(cs, GPIO_MODE_INPUT);
    return ok;
}

/* ---------- задача перебору ---------- */

static void probe_task(void *arg)
{
    s_found = false;
    s_progress = 0;
    s_total = NCAND * (NCAND - 1) * (NCAND - 2) * (NCAND - 3);
    for (int a = 0; a < NCAND && !s_found && !s_abort; a++)
      for (int b = 0; b < NCAND && !s_found && !s_abort; b++) {
        if (b == a) continue;
        for (int c = 0; c < NCAND && !s_found && !s_abort; c++) {
          if (c == a || c == b) continue;
          for (int d = 0; d < NCAND && !s_found && !s_abort; d++) {
            if (d == a || d == b || d == c) continue;
            s_progress++;
            if (try_card(CAND[a], CAND[b], CAND[c], CAND[d])) {
                s_sck = CAND[a]; s_mosi = CAND[b];
                s_miso = CAND[c]; s_cs = CAND[d];
                s_found = true;
            }
          }
        }
      }
    s_state = SP_DONE;
    vTaskDelete(NULL);
}

/* ---------- UI ---------- */

static void show_msg(const char *msg, uint32_t col)
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

static void render(void)
{
    if (s_state == SP_IDLE) {
        show_msg("Вставте microSD у слот.\n\nЦентр — почати пошук пінів.\n"
                 "(картку не пошкоджуємо — лише CMD0)", 0xC0C8D0);
    } else if (s_state == SP_RUN) {
        char m[64];
        snprintf(m, sizeof(m), "Перебір пінів...\n%d / %d", s_progress, s_total);
        show_msg(m, 0xFACC15);
    } else { /* SP_DONE */
        if (s_found) {
            char m[128];
            snprintf(m, sizeof(m),
                "Знайдено!\n\nCLK  = GPIO%d\nMOSI = GPIO%d\nMISO = GPIO%d\nCS   = GPIO%d\n\n"
                "центр — ще раз", s_sck, s_mosi, s_miso, s_cs);
            show_msg(m, 0x4ADE80);
            ESP_LOGI(APPTAG, "SD: SCK=%d MOSI=%d MISO=%d CS=%d",
                     s_sck, s_mosi, s_miso, s_cs);
        } else {
            show_msg("Картка не відповіла на жодній комбінації.\n\n"
                     "Перевір, чи вставлена картка, і спробуй ще (центр).", 0xF87171);
        }
    }
}

static void sp_tick(lv_timer_t *t)
{
    static sp_state_t last = -1;
    static int lastp = -1;
    if (s_state != last || (s_state == SP_RUN && s_progress != lastp)) {
        last = s_state; lastp = s_progress;
        render();
    }
}

static void start_probe(void)
{
    s_abort = false;
    s_state = SP_RUN;
    render();
    xTaskCreate(probe_task, "sdprobe", 4096, NULL, 4, NULL);
}

static void sp_open(lv_obj_t *scr)
{
    s_state = SP_IDLE;
    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, "SD-зонд");
    lv_obj_set_style_text_font(s_title, &font_ua_20, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 6);

    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 224, 200);
    lv_obj_align(s_root, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);

    render();
    s_timer = lv_timer_create(sp_tick, 200, NULL);
}

static void sp_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    /* перервати перебір і дочекатися завершення задачі (щоб не смикала GPIO) */
    s_abort = true;
    for (int i = 0; i < 100 && s_state == SP_RUN; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

static void sp_btn(int btn)
{
    if (btn != BTN_MID_ID) return;
    if (s_state == SP_IDLE || s_state == SP_DONE) start_probe();
}

const app_t app_sdprobe = {
    .name = "SD-зонд",
    .open = sp_open, .close = sp_close, .on_btn = sp_btn,
};

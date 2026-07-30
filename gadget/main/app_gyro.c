/*
 * MPU6050 (гіроскоп + акселерометр) на колодці GND/VCC/GPIO12=SCL/GPIO11=SDA
 * (той самий роз'єм, що й «Модулі»). Показує кутову швидкість X/Y/Z (°/с),
 * прискорення (g) і температуру. Центр — калібрування нуля гіроскопа
 * (пристрій має лежати нерухомо): усереднює N зразків і зберігає зсув у
 * NVS, він застосовується до всіх подальших показів.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "nvs.h"
#include "esp_log.h"
#include "lvgl.h"
#include "apps.h"

#define PIN_SDA 11
#define PIN_SCL 12
#define MPU_ADDR1 0x68
#define MPU_ADDR2 0x69

/* чутливість за замовчуванням: ±250 °/с -> 131 LSB/(°/с); ±2g -> 16384 LSB/g */
#define GYRO_LSB  131.0f
#define ACC_LSB   16384.0f

#define NS "gyro"

static const char *TAG = "mpu6050";

static i2c_master_bus_handle_t s_bus;
static bool s_bus_ok;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;
static bool s_found;

/* останні зчитані значення (калібровані) */
static float s_gx, s_gy, s_gz;      /* °/с */
static float s_ax, s_ay, s_az;      /* g */
static float s_temp;                /* °C */
static bool s_read_ok;

/* калібрувальний зсув гіроскопа (сирі LSB) */
static float s_off_x, s_off_y, s_off_z;
static bool s_calibrated;

/* стан калібрування: 0 idle, 1 йде, 2 готово (на короткий показ) */
static volatile int s_cal_state;
static volatile int s_cal_pct;

/* коректне завершення фонових задач перед закриттям I2C-шини (інакше вони
   продовжать звертатися до вже видаленого device-handle) */
static volatile bool s_closing;
static volatile bool s_read_running;
static volatile bool s_cal_running;

static lv_obj_t *s_root;
static lv_timer_t *s_timer;

/* ---------------- NVS: збереження зсуву ---------------- */

static void offs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t x = 0, y = 0, z = 0;
    bool ok = nvs_get_i32(h, "ox", &x) == ESP_OK &&
              nvs_get_i32(h, "oy", &y) == ESP_OK &&
              nvs_get_i32(h, "oz", &z) == ESP_OK;
    nvs_close(h);
    if (ok) {
        s_off_x = x / 1000.0f; s_off_y = y / 1000.0f; s_off_z = z / 1000.0f;
        s_calibrated = true;
    }
}

static void offs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "ox", (int32_t)(s_off_x * 1000));
    nvs_set_i32(h, "oy", (int32_t)(s_off_y * 1000));
    nvs_set_i32(h, "oz", (int32_t)(s_off_z * 1000));
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- I2C / MPU6050 ---------------- */

static void i2c_up(void)
{
    if (s_bus_ok) return;
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    s_bus_ok = (i2c_new_master_bus(&cfg, &s_bus) == ESP_OK);
}
static void i2c_down(void)
{
    if (s_bus_ok) { i2c_del_master_bus(s_bus); s_bus_ok = false; }
}

static bool mpu_wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 200) == ESP_OK;
}
static bool mpu_rd(uint8_t reg, uint8_t *buf, int len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 200) == ESP_OK;
}

static bool mpu_init(void)
{
    i2c_up();
    if (!s_bus_ok) return false;
    for (int i = 0; i < 2; i++) {
        uint8_t a = i == 0 ? MPU_ADDR1 : MPU_ADDR2;
        if (i2c_master_probe(s_bus, a, 100) != ESP_OK) continue;
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = a, .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) continue;
        uint8_t who = 0;
        if (mpu_rd(0x75, &who, 1) && (who & 0x7E) == 0x68) { s_addr = a; break; }
        i2c_master_bus_rm_device(s_dev); s_dev = NULL;
    }
    if (!s_dev) return false;
    mpu_wr(0x6B, 0x00);           /* PWR_MGMT_1: вихід зі сну, внутрішній тактовий */
    mpu_wr(0x1B, 0x00);           /* GYRO_CONFIG: ±250 °/с */
    mpu_wr(0x1C, 0x00);           /* ACCEL_CONFIG: ±2g */
    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

static bool mpu_read_raw(int16_t *ax, int16_t *ay, int16_t *az,
                         int16_t *t, int16_t *gx, int16_t *gy, int16_t *gz)
{
    uint8_t b[14];
    if (!mpu_rd(0x3B, b, 14)) return false;
    *ax = (b[0] << 8) | b[1];  *ay = (b[2] << 8) | b[3];  *az = (b[4] << 8) | b[5];
    *t  = (b[6] << 8) | b[7];
    *gx = (b[8] << 8) | b[9];  *gy = (b[10] << 8) | b[11]; *gz = (b[12] << 8) | b[13];
    return true;
}

static void gyro_read_task(void *arg)
{
    s_read_running = true;
    while (!s_closing) {
        int16_t ax, ay, az, t, gx, gy, gz;
        if (mpu_read_raw(&ax, &ay, &az, &t, &gx, &gy, &gz)) {
            s_ax = ax / ACC_LSB; s_ay = ay / ACC_LSB; s_az = az / ACC_LSB;
            s_temp = t / 340.0f + 36.53f;
            s_gx = (gx - s_off_x) / GYRO_LSB;
            s_gy = (gy - s_off_y) / GYRO_LSB;
            s_gz = (gz - s_off_z) / GYRO_LSB;
            s_read_ok = true;
        } else {
            s_read_ok = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_read_running = false;
    vTaskDelete(NULL);
}

/* усереднення N зразків для нового зсуву (пристрій має лежати нерухомо) */
static void calib_task(void *arg)
{
    s_cal_running = true;
    const int N = 60;             /* ~3 с при 50 мс кроці */
    int64_t sx = 0, sy = 0, sz = 0;
    int got = 0;
    for (int i = 0; i < N && !s_closing; i++) {
        int16_t ax, ay, az, t, gx, gy, gz;
        if (mpu_read_raw(&ax, &ay, &az, &t, &gx, &gy, &gz)) {
            sx += gx; sy += gy; sz += gz; got++;
        }
        s_cal_pct = (i + 1) * 100 / N;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_closing && got > 0) {
        s_off_x = (float)sx / got;
        s_off_y = (float)sy / got;
        s_off_z = (float)sz / got;
        s_calibrated = true;
        offs_save();
    }
    s_cal_state = s_closing ? 0 : 2;
    s_cal_running = false;
    vTaskDelete(NULL);
}

static void start_calib(void)
{
    if (!s_found || s_cal_state == 1) return;
    s_cal_state = 1;
    s_cal_pct = 0;
    xTaskCreate(calib_task, "gyrocal", 4096, NULL, 5, NULL);
}

/* ---------------- відображення ---------------- */

static void kv(lv_obj_t *parent, const char *label, const char *value,
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

static void render_missing(void)
{
    lv_obj_clean(s_root);
    lv_obj_set_layout(s_root, LV_LAYOUT_NONE);
    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, "MPU6050 не знайдено.\nПеревірте підключення\n"
                         "(11=SDA, 12=SCL)");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xF87171), 0);
    lv_obj_set_width(l, 220);
    lv_obj_center(l);
}

static void render_calibrating(void)
{
    char buf[16];
    lv_obj_clean(s_root);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_root, 12, 0);

    lv_obj_t *l = lv_label_create(s_root);
    lv_label_set_text(l, "Калібрування...\nне рухайте пристрій");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8ECF0), 0);

    lv_obj_t *bar = lv_bar_create(s_root);
    lv_obj_set_size(bar, 180, 14);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, s_cal_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x161D26), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x35C4F0), LV_PART_INDICATOR);

    lv_obj_t *pct = lv_label_create(s_root);
    snprintf(buf, sizeof(buf), "%d%%", s_cal_pct);
    lv_label_set_text(pct, buf);
    lv_obj_set_style_text_color(pct, lv_color_hex(0x35C4F0), 0);
}

static void render_live(void)
{
    char buf[24];
    lv_obj_clean(s_root);
    lv_obj_set_layout(s_root, LV_LAYOUT_NONE);

    lv_obj_t *hdr = lv_label_create(s_root);
    lv_label_set_text(hdr, "MPU6050");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xE8ECF0), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *col = lv_obj_create(s_root);
    lv_obj_set_size(col, 228, 188);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_opa(col, LV_OPA_0, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 5, 0);

    if (!s_read_ok) {
        lv_obj_t *l = lv_label_create(col);
        lv_label_set_text(l, "Немає звʼязку з датчиком");
        lv_obj_set_style_text_color(l, lv_color_hex(0xF87171), 0);
    } else {
        snprintf(buf, sizeof(buf), "%+6.1f °/с", s_gx); kv(col, "Гіро X", buf, 0x35C4F0);
        snprintf(buf, sizeof(buf), "%+6.1f °/с", s_gy); kv(col, "Гіро Y", buf, 0x35C4F0);
        snprintf(buf, sizeof(buf), "%+6.1f °/с", s_gz); kv(col, "Гіро Z", buf, 0x35C4F0);
        snprintf(buf, sizeof(buf), "%+.2f g", s_ax); kv(col, "Акс X", buf, 0x8A94A0);
        snprintf(buf, sizeof(buf), "%+.2f g", s_ay); kv(col, "Акс Y", buf, 0x8A94A0);
        snprintf(buf, sizeof(buf), "%+.2f g", s_az); kv(col, "Акс Z", buf, 0x8A94A0);
        snprintf(buf, sizeof(buf), "%.1f °C", s_temp); kv(col, "Температура", buf, 0xFFFFFF);
    }

    lv_obj_t *cal = lv_label_create(s_root);
    lv_label_set_text(cal, s_calibrated ? "Гіроскоп відкалібровано"
                                        : "Гіроскоп не калібрований");
    lv_obj_set_style_text_color(cal, lv_color_hex(
        s_calibrated ? 0x4ADE80 : 0x5A6672), 0);
    lv_obj_align(cal, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_obj_t *hint = lv_label_create(s_root);
    lv_label_set_text(hint, "Центр — калібрувати (нерухомо)");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x3A4550), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void app_poll(lv_timer_t *t)
{
    static int last = -2;
    if (!s_found) {
        if (last != -1) { render_missing(); last = -1; }
        return;
    }
    if (s_cal_state == 1) {
        render_calibrating();          /* оновлюємо прогрес щотик */
        last = 1;
        return;
    }
    if (s_cal_state == 2) s_cal_state = 0;   /* короткий показ готового — одразу live */
    render_live();                            /* показ значень щотик (живе оновлення) */
    last = 0;
}

static void gyro_open(lv_obj_t *scr)
{
    s_root = lv_obj_create(scr);
    lv_obj_set_size(s_root, 236, 236);
    lv_obj_center(s_root);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_scrollbar_mode(s_root, LV_SCROLLBAR_MODE_OFF);

    s_read_ok = false;
    s_cal_state = 0;
    s_closing = false;
    s_read_running = false;
    s_cal_running = false;
    offs_load();
    s_found = mpu_init();
    if (s_found) {
        render_live();
        xTaskCreate(gyro_read_task, "gyrord", 4096, NULL, 4, NULL);
    } else {
        ESP_LOGW(TAG, "MPU6050 не знайдено на 11/12");
        render_missing();
    }
    s_timer = lv_timer_create(app_poll, 150, NULL);
}

static void gyro_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    /* Дочекатися завершення фонових задач (читання/калібрування) ПЕРЕД
       видаленням I2C-девайса — інакше вони звернуться до вже вільного
       handle і впадуть. Обидві задачі перевіряють s_closing щоцикл і
       самі себе видаляють, тому чекаємо обмежений час. */
    s_closing = true;
    for (int i = 0; i < 50 && (s_read_running || s_cal_running); i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    s_closing = false;

    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    i2c_down();
    s_found = false;
}

static void gyro_btn(int btn)
{
    if (btn != BTN_MID_ID) return;
    if (s_cal_state == 1) return;    /* під час калібрування ігноруємо */
    start_calib();
}

const app_t app_gyro = {
    .name = "Гіроскоп (MPU6050)",
    .open = gyro_open, .close = gyro_close, .on_btn = gyro_btn,
};

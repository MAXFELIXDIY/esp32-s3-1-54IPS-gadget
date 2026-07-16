#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BLE_MAX_DEVS 50
#define BLE_ADV_MAX  62
#define BLE_MAX_CHARS 24

typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;
    int8_t   rssi;
    char     name[28];
    uint16_t company;          /* 0xFFFF якщо немає mfg data */
    int64_t  last_seen;
    uint8_t  adv[BLE_ADV_MAX];
    uint8_t  adv_len;
} ble_dev_t;

/* Вмикання/вимикання контролера BLE (Wi-Fi має бути вимкнений). */
void ble_start(void);
void ble_stop(void);
bool ble_is_ready(void);
const char *ble_stage(void);     /* поточний етап ініціалізації (для UI) */
void ble_scan_resume(void);      /* відновити сканування (після GATT) */
void ble_scan_stop(void);        /* зупинити сканування (список зафіксовано) */

int  ble_dev_snapshot(ble_dev_t *out, int max);   /* сортовано за RSSI */
void ble_dev_clear(void);

/* ---- GATT ---- */
typedef enum {
    GATT_IDLE, GATT_CONNECTING, GATT_DISCOVER, GATT_READING,
    GATT_DONE, GATT_FAIL
} gatt_state_t;

typedef struct {
    uint16_t uuid16;           /* 0 = 128-бітний (тоді дивись uuid128/is128) */
    bool     is128;
    uint8_t  uuid128[16];      /* повний UUID (little-endian, як у NimBLE) */
    uint16_t val_handle;
    uint8_t  props;
    uint8_t  val[40];
    uint8_t  val_len;
    bool     read_ok;
} ble_char_t;

void         ble_gatt_connect(const uint8_t *addr, uint8_t addr_type);
void         ble_gatt_disconnect(void);
gatt_state_t ble_gatt_state(void);
int          ble_gatt_chars(ble_char_t *out, int max);

/* Розшифровка відомої характеристики; true якщо UUID відомий. */
bool ble_decode_char(const ble_char_t *c, char *name, int nn,
                     char *value, int nv);

/* ---- Перехоплення сповіщень (notify/indicate) ---- */
#define BLE_MAX_NOTIF 24
typedef struct {
    uint16_t handle;           /* handle характеристики-джерела */
    uint16_t uuid16;           /* UUID (16-біт) якщо відомий, інакше 0 */
    uint8_t  val[40];
    uint8_t  val_len;
    int64_t  ts;               /* час (мкс) */
} ble_notif_t;

/* Підписатися на всі notify/indicate характеристики поточного з'єднання. */
void ble_gatt_subscribe_all(void);
/* Знімок перехоплених сповіщень (найновіші першими). Повертає кількість. */
int  ble_gatt_notif_snapshot(ble_notif_t *out, int max);
/* Скільки сповіщень перехоплено всього (лічильник, для індикації "живого" потоку). */
uint32_t ble_gatt_notif_count(void);

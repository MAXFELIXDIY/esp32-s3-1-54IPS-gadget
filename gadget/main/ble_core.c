/*
 * BLE-ядро (NimBLE): вмикається на вимогу (лише для сканера, коли Wi-Fi
 * вимкнено). Пасивне сканування + таблиця пристроїв + GATT-клієнт із
 * розшифровкою стандартних характеристик (батарея, пульс, температура...).
 */
#include <string.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "ble_core.h"

static const char *TAG = "ble";

static ble_dev_t s_devs[BLE_MAX_DEVS];
static int s_n_devs = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_ready = false;
static bool s_inited = false;
static volatile bool s_scanning = false;
static uint8_t s_own_addr_type;
static const char *s_stage = "—";   /* етап для UI-діагностики */

const char *ble_stage(void) { return s_stage; }

/* ---------------- сканування ---------------- */

static int scan_cb(struct ble_gap_event *ev, void *arg)
{
    if (ev->type != BLE_GAP_EVENT_DISC) return 0;
    struct ble_hs_adv_fields f;
    char name[28] = "";
    uint16_t company = 0xFFFF;
    if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) == 0) {
        if (f.name && f.name_len) {
            int n = f.name_len < 27 ? f.name_len : 27;
            memcpy(name, f.name, n); name[n] = 0;
        }
        if (f.mfg_data && f.mfg_data_len >= 2)
            company = f.mfg_data[0] | (f.mfg_data[1] << 8);
    }
    portENTER_CRITICAL(&s_lock);
    int idx = -1;
    /* 1) збіг за MAC */
    for (int i = 0; i < s_n_devs; i++)
        if (!memcmp(s_devs[i].addr, ev->disc.addr.val, 6)) { idx = i; break; }
    /* 2) інакше — збіг за назвою (пристрої з випадковими адресами) */
    if (idx < 0 && name[0])
        for (int i = 0; i < s_n_devs; i++)
            if (!strcmp(s_devs[i].name, name)) { idx = i; break; }
    if (idx < 0 && s_n_devs < BLE_MAX_DEVS) idx = s_n_devs++;
    if (idx >= 0) {
        ble_dev_t *d = &s_devs[idx];
        memcpy(d->addr, ev->disc.addr.val, 6);
        d->addr_type = ev->disc.addr.type;
        d->rssi = ev->disc.rssi;
        if (name[0]) strcpy(d->name, name);
        d->company = company;
        d->last_seen = esp_timer_get_time();
        d->adv_len = ev->disc.length_data < BLE_ADV_MAX ? ev->disc.length_data : BLE_ADV_MAX;
        memcpy(d->adv, ev->disc.data, d->adv_len);
    }
    portEXIT_CRITICAL(&s_lock);
    return 0;
}

void ble_scan_resume(void)
{
    if (!s_ready || s_scanning) return;
    /* активне сканування: надсилаємо scan-запити, щоб отримати scan response
       з назвами пристроїв (багато пристроїв віддають назву лише там) */
    struct ble_gap_disc_params p = {
        .passive = 0, .itvl = 0x50, .window = 0x40, .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, scan_cb, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) s_scanning = true;
}

void ble_scan_stop(void)
{
    if (s_scanning) { ble_gap_disc_cancel(); s_scanning = false; }
}

int ble_dev_snapshot(ble_dev_t *out, int max)
{
    portENTER_CRITICAL(&s_lock);
    int n = s_n_devs < max ? s_n_devs : max;
    memcpy(out, s_devs, sizeof(ble_dev_t) * n);
    portEXIT_CRITICAL(&s_lock);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) {
            ble_dev_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

void ble_dev_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_n_devs = 0;
    portEXIT_CRITICAL(&s_lock);
}

/* ---------------- GATT ---------------- */

static volatile gatt_state_t s_gs = GATT_IDLE;
static uint16_t s_conn = 0;
static ble_char_t s_chars[BLE_MAX_CHARS];
static uint16_t s_char_handle[BLE_MAX_CHARS];
static int s_n_chars = 0, s_read_idx = 0;

static void start_next_read(void);

static int read_cb(uint16_t conn, const struct ble_gatt_error *err,
                   struct ble_gatt_attr *attr, void *arg)
{
    int i = (int)(intptr_t)arg;
    if (err->status == 0 && attr && i < s_n_chars) {
        int n = OS_MBUF_PKTLEN(attr->om);
        if (n > 40) n = 40;
        ble_hs_mbuf_to_flat(attr->om, s_chars[i].val, n, NULL);
        s_chars[i].val_len = n;
        s_chars[i].read_ok = true;
    }
    s_read_idx++;
    start_next_read();
    return 0;
}

static void start_next_read(void)
{
    while (s_read_idx < s_n_chars) {
        if (s_chars[s_read_idx].props & BLE_GATT_CHR_PROP_READ) {
            if (ble_gattc_read(s_conn, s_char_handle[s_read_idx],
                               read_cb, (void *)(intptr_t)s_read_idx) == 0)
                return;
        }
        s_read_idx++;
    }
    s_gs = GATT_DONE;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr && s_n_chars < BLE_MAX_CHARS) {
        ble_char_t *c = &s_chars[s_n_chars];
        memset(c, 0, sizeof(*c));
        if (chr->uuid.u.type == BLE_UUID_TYPE_16)
            c->uuid16 = BLE_UUID16(&chr->uuid)->value;
        c->props = chr->properties;
        s_char_handle[s_n_chars] = chr->val_handle;
        s_n_chars++;
    } else if (err->status == BLE_HS_EDONE) {
        s_gs = GATT_READING;
        s_read_idx = 0;
        start_next_read();
    }
    return 0;
}

static int gatt_cb(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_n_chars = 0;
            s_gs = GATT_DISCOVER;
            ble_gattc_disc_all_chrs(s_conn, 1, 0xffff, chr_cb, NULL);
        } else {
            s_gs = GATT_FAIL;
            ble_scan_resume();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (s_gs != GATT_DONE && s_gs != GATT_FAIL) s_gs = GATT_FAIL;
        ble_scan_resume();
        break;
    default: break;
    }
    return 0;
}

void ble_gatt_connect(const uint8_t *addr, uint8_t addr_type)
{
    if (s_scanning) { ble_gap_disc_cancel(); s_scanning = false; }
    s_gs = GATT_CONNECTING;
    s_n_chars = 0;
    ble_addr_t peer = { .type = addr_type };
    memcpy(peer.val, addr, 6);
    if (ble_gap_connect(s_own_addr_type, &peer, 8000, NULL, gatt_cb, NULL)) {
        s_gs = GATT_FAIL;
        ble_scan_resume();
    }
}

void ble_gatt_disconnect(void)
{
    if (s_gs == GATT_CONNECTING || s_gs == GATT_DISCOVER ||
        s_gs == GATT_READING || s_gs == GATT_DONE)
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    s_gs = GATT_IDLE;
}

gatt_state_t ble_gatt_state(void) { return s_gs; }

int ble_gatt_chars(ble_char_t *out, int max)
{
    int n = s_n_chars < max ? s_n_chars : max;
    memcpy(out, s_chars, sizeof(ble_char_t) * n);
    return n;
}

bool ble_decode_char(const ble_char_t *c, char *name, int nn, char *val, int nv)
{
    const uint8_t *v = c->val; int len = c->val_len;
    switch (c->uuid16) {
    case 0x2A00: snprintf(name, nn, "Назва"); snprintf(val, nv, "%.*s", len, v); return true;
    case 0x2A29: snprintf(name, nn, "Виробник"); snprintf(val, nv, "%.*s", len, v); return true;
    case 0x2A24: snprintf(name, nn, "Модель"); snprintf(val, nv, "%.*s", len, v); return true;
    case 0x2A26: snprintf(name, nn, "Версія ПЗ"); snprintf(val, nv, "%.*s", len, v); return true;
    case 0x2A19:
        snprintf(name, nn, "Заряд батареї");
        snprintf(val, nv, len >= 1 ? "%d%%" : "?", len >= 1 ? v[0] : 0);
        return true;
    case 0x2A6E:
        snprintf(name, nn, "Температура");
        if (len >= 2) snprintf(val, nv, "%.2f°C", (int16_t)(v[0] | (v[1] << 8)) * 0.01);
        return true;
    case 0x2A6F:
        snprintf(name, nn, "Вологість");
        if (len >= 2) snprintf(val, nv, "%.2f%%", (uint16_t)(v[0] | (v[1] << 8)) * 0.01);
        return true;
    case 0x2A37:
        snprintf(name, nn, "Пульс");
        if (len >= 2) snprintf(val, nv, "%d уд/хв", (v[0] & 1) ? (v[1] | (v[2] << 8)) : v[1]);
        return true;
    case 0x2A53:
        snprintf(name, nn, "Кроки/актив.");
        if (len >= 2) snprintf(val, nv, "%d", v[0] | (v[1] << 8));
        return true;
    default: return false;
    }
}

/* ---------------- увімкнення / вимкнення ---------------- */

static void on_sync(void)
{
    s_stage = "sync ok";
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_ready = true;
    ble_scan_resume();
    ESP_LOGI(TAG, "NimBLE готовий, сканую");
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(void)
{
    if (s_inited) return;
    s_stage = "port_init";
    esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) {
        s_stage = "port_init FAIL";
        ESP_LOGE(TAG, "port_init fail: %s", esp_err_to_name(rc));
        return;
    }
    s_stage = "host start";
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = NULL;
    nimble_port_freertos_init(host_task);
    s_inited = true;
    ble_dev_clear();
    s_stage = "wait sync";
    for (int i = 0; i < 60 && !s_ready; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_ready) s_stage = "sync TIMEOUT";
}

void ble_stop(void)
{
    if (!s_inited) return;
    if (s_scanning) { ble_gap_disc_cancel(); s_scanning = false; }
    ble_gatt_disconnect();
    nimble_port_stop();
    nimble_port_deinit();
    s_inited = false; s_ready = false; s_gs = GATT_IDLE;
    ESP_LOGI(TAG, "NimBLE зупинено");
}

bool ble_is_ready(void) { return s_ready; }

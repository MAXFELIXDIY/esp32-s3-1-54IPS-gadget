#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "netcfg.h"

static const char *TAG = "netcfg";

static EventGroupHandle_t s_ev;
#define BIT_GOT_IP   BIT0
#define BIT_FAIL     BIT1

static volatile bool s_connected = false;
static bool s_want_connect = false;
static char s_ssid[33] = "";

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_want_connect) {
            esp_wifi_connect();  /* автоперепідключення */
            xEventGroupSetBits(s_ev, BIT_FAIL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
        /* синхронізація часу при кожному підключенні */
        esp_netif_sntp_start();
    }
}

void netcfg_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_event, NULL, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* часовий пояс Києва + SNTP (запуск за подією GOT_IP) */
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.start = false;   /* стартуємо вручну при GOT_IP */
    esp_netif_sntp_init(&sc);
}

void netcfg_get_clock(char *out, size_t sz)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year < 121)       /* до 2021 → час не синхронізований */
        snprintf(out, sz, "--:--");
    else
        snprintf(out, sz, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

bool netcfg_load(char *ssid, size_t ssz, char *pass, size_t psz)
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, "ssid", ssid, &ssz) == ESP_OK &&
              nvs_get_str(h, "pass", pass, &psz) == ESP_OK && ssid[0];
    nvs_close(h);
    return ok;
}

#define WIFI_NS "wifi"     /* сховище паролів за SSID */
#define WIFI_MAX 16

static int find_idx(nvs_handle_t h, const char *ssid)
{
    int32_t cnt = 0;
    nvs_get_i32(h, "cnt", &cnt);
    char key[16], val[33];
    for (int i = 0; i < cnt && i < WIFI_MAX; i++) {
        size_t sz = sizeof(val);
        snprintf(key, sizeof(key), "s%d", i);
        if (nvs_get_str(h, key, val, &sz) == ESP_OK && !strcmp(val, ssid))
            return i;
    }
    return -1;
}

void netcfg_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    /* «остання» мережа — для автопідключення при старті */
    if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
    /* список усіх мереж — для позначок і швидкого конекту */
    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) != ESP_OK) return;
    int i = find_idx(h, ssid);
    if (i < 0) {
        int32_t cnt = 0;
        nvs_get_i32(h, "cnt", &cnt);
        if (cnt < WIFI_MAX) { i = cnt; nvs_set_i32(h, "cnt", cnt + 1); }
        else i = WIFI_MAX - 1;   /* переповнення — перезапис останнього */
    }
    char key[16];
    snprintf(key, sizeof(key), "s%d", i); nvs_set_str(h, key, ssid);
    snprintf(key, sizeof(key), "p%d", i); nvs_set_str(h, key, pass);
    nvs_commit(h);
    nvs_close(h);
}

bool netcfg_get_pass(const char *ssid, char *pass, size_t psz)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READONLY, &h) != ESP_OK) return false;
    int i = find_idx(h, ssid);
    bool ok = false;
    if (i >= 0) {
        char key[16];
        snprintf(key, sizeof(key), "p%d", i);
        ok = nvs_get_str(h, key, pass, &psz) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

bool netcfg_has_saved(const char *ssid)
{
    char p[65];
    return netcfg_get_pass(ssid, p, sizeof(p));
}

int netcfg_scan(wifi_ap_record_t *out, int max)
{
    /* Зупиняємо автоперепідключення: інакше безперервні спроби connect до
       збереженої мережі (коли її немає поруч) блокують сканування і воно
       повертає порожній список. */
    s_want_connect = false;
    esp_wifi_disconnect();
    esp_wifi_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(120));

    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t n = max;
    esp_wifi_scan_get_ap_records(&n, out);
    ESP_LOGI(TAG, "знайдено %d мереж", n);
    return n;
}

bool netcfg_connect(const char *ssid, const char *pass, int timeout_ms)
{
    s_want_connect = false;   /* зупиняємо автоперепідключення на час зміни */
    esp_wifi_disconnect();

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAIL);
    s_want_connect = true;
    esp_wifi_connect();

    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_GOT_IP, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    if (b & BIT_GOT_IP) return true;
    return false;
}

bool netcfg_is_connected(void) { return s_connected; }
const char *netcfg_ssid(void) { return s_ssid; }

static bool s_wifi_on = true;   /* після netcfg_init радіо увімкнене */

/* Повний деініт драйвера Wi-Fi — звільняє десятки КБ внутрішньої RAM для BLE. */
void netcfg_wifi_stop(void)
{
    if (!s_wifi_on) return;
    s_want_connect = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_connected = false;
    s_wifi_on = false;
    ESP_LOGI(TAG, "Wi-Fi драйвер деініціалізовано (RAM звільнено)");
}

void netcfg_wifi_resume(void)
{
    if (s_wifi_on) return;
    /* повторна ініціалізація драйвера (netif + обробники подій уже створені) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) { ESP_LOGE(TAG, "wifi_init fail"); return; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_start();
    s_wifi_on = true;
    ESP_LOGI(TAG, "Wi-Fi драйвер відновлено");
    /* підключення до збереженої мережі (асинхронно) */
    char ssid[33], pass[65];
    if (netcfg_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        s_want_connect = true;
        esp_wifi_connect();
    }
}

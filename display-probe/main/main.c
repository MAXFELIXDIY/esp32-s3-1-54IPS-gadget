/* Тест ключа Groq: chat completion (llama), лог статусу і відповіді. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "gemini_key.h"

static const char *TAG = "groqtest";
#define SSID "MAXFELIX"
#define PASS "12maxfelix03"
#define URL "https://api.groq.com/openai/v1/chat/completions"

static EventGroupHandle_t ev;
static void on_ev(void *a, esp_event_base_t b, int32_t id, void *d)
{
    if (b == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (b == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (b == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(ev, 1);
}

void app_main(void)
{
    nvs_flash_init();
    ev = xEventGroupCreate();
    esp_netif_init(); esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&ic);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_ev, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ev, NULL, NULL);
    wifi_config_t wc = {0};
    strcpy((char *)wc.sta.ssid, SSID); strcpy((char *)wc.sta.password, PASS);
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
    xEventGroupWaitBits(ev, 1, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    ESP_LOGI(TAG, "Wi-Fi готово");

    const char *body =
      "{\"model\":\"llama-3.3-70b-versatile\",\"messages\":["
      "{\"role\":\"user\",\"content\":\"Привітайся українською одним реченням.\"}]}";
    esp_http_client_config_t cfg = {
        .url = URL, .method = HTTP_METHOD_POST, .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Authorization", "Bearer " GEMINI_KEY);
    int status = -1;
    static char resp[4096]; int t = 0;
    if (esp_http_client_open(c, strlen(body)) == ESP_OK) {
        esp_http_client_write(c, body, strlen(body));
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        int n;
        while (t < (int)sizeof(resp) - 1 &&
               (n = esp_http_client_read(c, resp + t, sizeof(resp) - 1 - t)) > 0) t += n;
        resp[t] = 0;
    }
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "HTTP статус: %d", status);
    ESP_LOGI(TAG, "відповідь (%d б): %.1200s", t, resp);

    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}

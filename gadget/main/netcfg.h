#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi.h"

/* Ініціалізація NVS + Wi-Fi у режимі STA (без підключення). */
void netcfg_init(void);

/* Завантажити збережені SSID/пароль. true, якщо є. */
bool netcfg_load(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);
/* Зберегти SSID/пароль у NVS. */
void netcfg_save(const char *ssid, const char *pass);

/* Блокуюче сканування. Заповнює масив, повертає кількість. */
int netcfg_scan(wifi_ap_record_t *out, int max);

/* Підключення (блокує до IP або таймауту). */
bool netcfg_connect(const char *ssid, const char *pass, int timeout_ms);

bool netcfg_is_connected(void);
const char *netcfg_ssid(void);   /* поточний SSID або "" */

/* Керування живленням радіо Wi-Fi (енергоощадність / звільнення для BLE). */
void netcfg_wifi_stop(void);     /* вимкнути радіо Wi-Fi */
void netcfg_wifi_resume(void);   /* увімкнути й підключитися до збереженої */

/* Поточний час "ГГ:ХХ" або "--:--" якщо не синхронізовано. */
void netcfg_get_clock(char *out, size_t sz);

/* Багатомережеве сховище паролів (за SSID). */
bool netcfg_has_saved(const char *ssid);
bool netcfg_get_pass(const char *ssid, char *pass, size_t pass_sz);

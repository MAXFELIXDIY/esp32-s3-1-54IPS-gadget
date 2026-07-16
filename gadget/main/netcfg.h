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

/* ---- Сирі 802.11-операції (Wi-Fi-інструменти) ---- */
/* Підготувати радіо до атаки на точку bssid/channel: зупинити автопідключення,
   зафіксувати канал і почати «підслуховувати» клієнтів точки (promiscuous). */
void netcfg_raw_begin(const uint8_t bssid[6], uint8_t channel);
/* Відновити звичайний режим STA (автопідключення до збереженої мережі). */
void netcfg_raw_end(void);
/* Надіслати «пачку» deauth/disassoc: адресно кожному виявленому клієнту (в
   обидва боки) + широкомовно. Повертає кількість надісланих кадрів. Лише для
   власних мереж / авторизованого тестування. */
int  netcfg_deauth(int bursts);
/* Скільки клієнтів точки виявлено «підслуховуванням». */
int  netcfg_deauth_clients(void);
/* Знімок списку MAC-адрес клієнтів. Повертає кількість (≤ max). */
int  netcfg_client_snapshot(uint8_t out[][6], int max);
/* Адресна атака на одного клієнта (в обидва боки). Повертає к-сть кадрів. */
int  netcfg_deauth_one(const uint8_t client[6], int bursts);

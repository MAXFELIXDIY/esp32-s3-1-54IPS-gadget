#pragma once
#include <stdbool.h>

/* Підключення до Wi-Fi (блокує до готовності або таймауту). */
bool wifi_connect_blocking(const char *ssid, const char *pass, int timeout_ms);

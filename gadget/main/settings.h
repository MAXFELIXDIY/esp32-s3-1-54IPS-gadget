#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Завантажити збережені параметри й застосувати (яскравість, гучність). */
void settings_init(void);

uint8_t settings_brightness(void);      /* 15..255 */
void    settings_set_brightness(int v); /* застосовує + зберігає */

int  settings_volume(void);             /* 0..256 */
void settings_set_volume(int v);        /* застосовує + зберігає */

/* Видимість застосунку i в меню (бітова маска, за замовч. усі увімкнені). */
bool settings_menu_visible(int i);
void settings_menu_toggle(int i);       /* перемкнути й зберегти */

/* URL образу прошивки для FOTA. Порожній рядок = не задано. */
const char *settings_fota_url(void);
void        settings_set_fota_url(const char *url); /* зберігає в NVS */

/* Ключ Groq API. Зберігається в NVS (не в бінарнику). Порожній = не задано. */
const char *settings_groq_key(void);
void        settings_set_groq_key(const char *key); /* зберігає в NVS */

#pragma once
#include <stdint.h>

/* Завантажити збережені параметри й застосувати (яскравість, гучність). */
void settings_init(void);

uint8_t settings_brightness(void);      /* 15..255 */
void    settings_set_brightness(int v); /* застосовує + зберігає */

int  settings_volume(void);             /* 0..256 */
void settings_set_volume(int v);        /* застосовує + зберігає */

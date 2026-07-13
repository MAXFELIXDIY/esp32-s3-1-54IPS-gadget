#pragma once
#include <stdbool.h>

void battery_init(void);
void battery_sample(void);   /* зчитати ADC + статус заряджання, оновити кеш */
int  battery_level(void);    /* 0..100, або -1 якщо ще невідомо */
bool battery_charging(void);

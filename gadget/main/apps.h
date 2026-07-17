#pragma once
#include "lvgl.h"

#define BTN_LEFT_ID  0
#define BTN_MID_ID   1
#define BTN_RIGHT_ID 2

typedef struct {
    const char *name;
    void (*open)(lv_obj_t *scr);
    void (*close)(void);
    void (*on_btn)(int btn);
    /* необовʼязково: утримання бокової кнопки (–/+). Якщо задано, для цього
       застосунку бокові кнопки спрацьовують по відпусканню (короткий тап),
       а утримання ~700 мс віддає окрему подію сюди. NULL = стара поведінка
       (клік по фронту + автоповтор). */
    void (*on_hold)(int btn);
} app_t;

extern const app_t app_wifi;
extern const app_t app_weather;
extern const app_t app_radio;
extern const app_t app_news;
extern const app_t app_settings;
extern const app_t app_gemini;
extern const app_t app_ble;
extern const app_t app_wifitools;
extern const app_t app_sdprobe;
extern const app_t app_pet;
extern const app_t app_pipboy;
extern const app_t app_calendar;
extern const app_t app_calc;
extern const app_t app_modules;
extern const app_t app_light;
extern const app_t app_fota;

/* керування підсвіткою (реалізовано в main.c) */
void ui_backlight_set(uint8_t duty);

/* доступ до списку застосунків для кастомізації меню (реалізовано в main.c) */
int         menu_app_count(void);
const char *menu_app_name(int i);
bool        menu_app_locked(int i);   /* асистент і налаштування — завжди у меню */

LV_FONT_DECLARE(font_ua_16);
LV_FONT_DECLARE(font_ua_20);
LV_FONT_DECLARE(font_num_34);

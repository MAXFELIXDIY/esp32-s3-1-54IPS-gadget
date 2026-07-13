/* Постійні налаштування користувача: яскравість екрана і гучність. */
#include "nvs.h"
#include "esp_log.h"
#include "apps.h"
#include "audio.h"
#include "settings.h"

#define NS "cfg"
#define BR_MIN 15
#define BR_MAX 255

static uint8_t s_bright = 200;
static int s_vol = 200;

static void save_u8(const char *k, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, k, v); nvs_commit(h); nvs_close(h);
}
static void save_i32(const char *k, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, k, v); nvs_commit(h); nvs_close(h);
}

void settings_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "bright", &s_bright);
        int32_t v = s_vol;
        if (nvs_get_i32(h, "vol", &v) == ESP_OK) s_vol = v;
        nvs_close(h);
    }
    if (s_bright < BR_MIN) s_bright = BR_MIN;
    ui_backlight_set(s_bright);
    audio_set_volume(s_vol);
}

uint8_t settings_brightness(void) { return s_bright; }

void settings_set_brightness(int v)
{
    if (v < BR_MIN) v = BR_MIN;
    if (v > BR_MAX) v = BR_MAX;
    s_bright = v;
    ui_backlight_set(s_bright);
    save_u8("bright", s_bright);
}

int settings_volume(void) { return s_vol; }

void settings_set_volume(int v)
{
    if (v < 0) v = 0;
    if (v > 256) v = 256;
    s_vol = v;
    audio_set_volume(s_vol);
    save_i32("vol", s_vol);
}

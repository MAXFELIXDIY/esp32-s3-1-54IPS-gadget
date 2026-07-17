/* Постійні налаштування користувача: яскравість екрана і гучність. */
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "apps.h"
#include "audio.h"
#include "settings.h"

#ifdef PROVISION_SECRETS
#include "groq_key.h"          /* лише для провізії; GROQ_KEY -> NVS */
#define GROQ_KEY_SEED GROQ_KEY
#endif

#define NS "cfg"
#define BR_MIN 15
#define BR_MAX 255

/* URL за замовчуванням для FOTA. Стабільне посилання на gadget.bin із
   найновішого GitHub-релізу (не змінюється між релізами — достатньо
   опублікувати новий реліз). Значення в NVS має пріоритет над цим. */
#define FOTA_URL_DEFAULT \
    "https://github.com/MAXFELIXDIY/esp32-s3-1-54IPS-gadget/releases/latest/download/gadget.bin"

static uint8_t s_bright = 200;
static int s_vol = 200;
static uint32_t s_menumask = 0xFFFFFFFF;   /* усі застосунки видимі за замовч. */
static char s_fota_url[160] = FOTA_URL_DEFAULT;
static char s_groq_key[80] = "";           /* лише з NVS — у бінарник не пишемо */

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
        nvs_get_u32(h, "menumask", &s_menumask);
        size_t sz = sizeof(s_fota_url);
        nvs_get_str(h, "fota_url", s_fota_url, &sz);
        sz = sizeof(s_groq_key);
        nvs_get_str(h, "groq_key", s_groq_key, &sz);
        nvs_close(h);
    }

#ifdef PROVISION_SECRETS
    /* Разова «провізія» секретів у NVS. Складається лише за наявності
       -DPROVISION_SECRETS (див. README/FOTA). Тільки такий локальний білд
       містить ключ у бінарнику; звичайні збірки й OTA-образи — ні. */
    if (s_groq_key[0] == 0) {
        settings_set_groq_key(GROQ_KEY_SEED);
    }
#endif
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

bool settings_menu_visible(int i)
{
    return (s_menumask >> i) & 1u;
}

void settings_menu_toggle(int i)
{
    s_menumask ^= (1u << i);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "menumask", s_menumask); nvs_commit(h); nvs_close(h);
}

const char *settings_groq_key(void) { return s_groq_key; }

void settings_set_groq_key(const char *key)
{
    if (!key) return;
    strlcpy(s_groq_key, key, sizeof(s_groq_key));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "groq_key", s_groq_key); nvs_commit(h); nvs_close(h);
}

const char *settings_fota_url(void) { return s_fota_url; }

void settings_set_fota_url(const char *url)
{
    if (!url) return;
    strlcpy(s_fota_url, url, sizeof(s_fota_url));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "fota_url", s_fota_url); nvs_commit(h); nvs_close(h);
}

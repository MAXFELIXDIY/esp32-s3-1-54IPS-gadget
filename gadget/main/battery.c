/*
 * Моніторинг батареї (плата XingZhi Cube 1.54):
 *   напруга — ADC2 канал 6 (GPIO17), дільник; статус заряджання — GPIO38.
 * Таблиця ADC->% з референсного power_manager.h.
 * ADC2 ділиться з Wi-Fi, тож зчитування може інколи не вдатись — тоді
 * лишаємо попереднє значення.
 */
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "battery.h"

#define CHARGING_GPIO 38

static adc_oneshot_unit_handle_t s_adc;
static bool s_ok = false;
static int s_level = -1;
static bool s_charging = false;

void battery_init(void)
{
    gpio_config_t io = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << CHARGING_GPIO,
    };
    gpio_config(&io);

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_2 };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) return;
    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, ADC_CHANNEL_6, &ccfg) == ESP_OK)
        s_ok = true;
}

void battery_sample(void)
{
    if (!s_ok) return;
    s_charging = gpio_get_level(CHARGING_GPIO) == 1;

    long sum = 0; int n = 0;
    for (int i = 0; i < 3; i++) {
        int v;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_6, &v) == ESP_OK) { sum += v; n++; }
    }
    if (n == 0) return;              /* конфлікт із Wi-Fi — лишаємо старе */
    int adc = sum / n;

    static const struct { int adc; int lvl; } L[] = {
        {1970, 0}, {2062, 20}, {2154, 40}, {2246, 60}, {2338, 80}, {2430, 100}
    };
    if (adc < L[0].adc) s_level = 0;
    else if (adc >= L[5].adc) s_level = 100;
    else for (int i = 0; i < 5; i++)
        if (adc >= L[i].adc && adc < L[i + 1].adc) {
            float r = (float)(adc - L[i].adc) / (L[i + 1].adc - L[i].adc);
            s_level = L[i].lvl + (int)(r * (L[i + 1].lvl - L[i].lvl));
            break;
        }
}

int  battery_level(void) { return s_level; }
bool battery_charging(void) { return s_charging; }

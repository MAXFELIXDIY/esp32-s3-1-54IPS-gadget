/* Адресний RGB-світлодіод WS2812 (GPIO48) через RMT. */
#include "led_strip.h"
#include "led.h"

#define LED_PIN 48

static led_strip_handle_t s_led;
static bool s_ok;

void led_init(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num = LED_PIN, .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    s_ok = (led_strip_new_rmt_device(&sc, &rc, &s_led) == ESP_OK);
    if (s_ok) led_strip_clear(s_led);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ok) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

void led_off(void)
{
    if (s_ok) led_strip_clear(s_led);
}

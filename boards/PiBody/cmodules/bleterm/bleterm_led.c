// LED status (SPEC 5.5). The worker computes the wanted level every 50 ms tick and writes the
// CYW43 GPIO (an SPI transaction to the radio) only when the level changes. Scheduler context
// only: never from an IRQ.
#include "bleterm_led.h"

int led_level(led_state_t state, uint32_t t_ms) {
    uint32_t ph = t_ms % 1000;
    switch (state) {
        case LED_PAIRING:
            return (t_ms % 200) < 100;
        case LED_ADVERTISING:
            return ph < 100;
        case LED_UNAUTH:
            return ph < 100 || (ph >= 200 && ph < 300);
        case LED_AUTH:
            return 1;
        default:
            return 0;
    }
}

#ifndef BLETERM_LED_HOST

#include "py/mphal.h"
#include "lib/cyw43-driver/src/cyw43.h"
#include "bleterm_int.h"

#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0
#endif

static int8_t led_written = -1; // level last written by bleterm, -1 = not driving

static led_state_t led_state(void) {
    if (!bleterm_g.started) {
        return LED_OFF;
    }
    if (bleterm_g.pairing_until_ms != 0) {
        return LED_PAIRING;
    }
    if (bleterm_g.ide >= 0) {
        return bleterm_g.links[(int)bleterm_g.ide].auth ? LED_AUTH : LED_UNAUTH;
    }
    if (!mp_bluetooth_btstack_user_state_present()) {
        for (int i = 0; i < BLETERM_MAX_LINKS; ++i) {
            if (bleterm_g.links[i].handle != BLETERM_CON_NONE) {
                return LED_UNAUTH;
            }
        }
    }
    return LED_ADVERTISING;
}

void bleterm_led_worker(void) {
    if (!bleterm_g.led_enabled || bleterm_g.led_released) {
        led_written = -1;
        return;
    }
    led_state_t st = led_state();
    int level;
    if (st == LED_OFF) {
        if (led_written <= 0) {
            led_written = -1; // never drive it while off
            return;
        }
        level = 0;  // turn off what we left on, then stop driving
    } else {
        level = led_level(st, mp_hal_ticks_ms());
    }
    if (level != led_written) {
        cyw43_gpio_set(&cyw43_state, CYW43_WL_GPIO_LED_PIN, level);
        led_written = st == LED_OFF ? -1 : level;
    }
}

void bleterm_led_release(void) {
    bleterm_g.led_released = true;
    led_written = -1;
}

#endif

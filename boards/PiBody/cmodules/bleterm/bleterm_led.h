// LED status patterns (SPEC 5.5). led_level() is pure and also built for host tests.
#ifndef BLETERM_LED_H
#define BLETERM_LED_H

#include <stdint.h>

typedef enum {
    LED_OFF = 0,        // BLE off: bleterm does not drive the LED
    LED_PAIRING,        // pairing window open: 5 Hz flash
    LED_ADVERTISING,    // no IDE link: 100 ms on every 1 s
    LED_UNAUTH,         // link connected, not authenticated: two 100 ms blinks every 1 s
    LED_AUTH,           // IDE link authenticated: on
} led_state_t;

int led_level(led_state_t state, uint32_t t_ms);

#endif

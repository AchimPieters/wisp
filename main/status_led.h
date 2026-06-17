#pragma once

// States shown by the on-board WS2812 RGB LED:
typedef enum {
    LED_SETUP,        // setup mode (captive portal)            -> blue
    LED_CONNECTING,   // connecting to the upstream network     -> blinking orange
    LED_ONLINE,       // upstream connected, internet shared    -> green
    LED_OFFLINE,      // upstream lost in operational mode       -> blinking red
} led_state_t;

void status_led_init(void);
void status_led_set(led_state_t state);

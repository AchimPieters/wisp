#pragma once

// Statussen die de WS2812 op de C6 Super Mini (GPIO8) toont:
typedef enum {
    LED_SETUP,        // setup-modus (captive portal)  -> blauw
    LED_CONNECTING,   // verbinden met upstream        -> oranje knipperend
    LED_ONLINE,       // upstream verbonden, internet gedeeld -> groen
    LED_OFFLINE,      // upstream weg in operationele modus  -> rood knipperend
} led_state_t;

void status_led_init(void);
void status_led_set(led_state_t state);

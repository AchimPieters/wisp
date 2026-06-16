#include "status_led.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define LED_GPIO 8          // WS2812 op de meeste ESP32-C6 Super Mini's

static const char *TAG = "led";
static led_strip_handle_t s_strip;
static volatile led_state_t s_state = LED_SETUP;

static void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

// Eigen taakje: zet de kleur op basis van de huidige status (vast of knipperend).
// Helderheid bewust laag gehouden; de onboard-LED is fel.
static void led_task(void *pv) {
    bool on = false;
    while (1) {
        switch (s_state) {
            case LED_SETUP:                       // rustig blauw
                set_rgb(0, 0, 40);
                vTaskDelay(pdMS_TO_TICKS(400));
                break;
            case LED_ONLINE:                      // vast groen
                set_rgb(0, 40, 0);
                vTaskDelay(pdMS_TO_TICKS(400));
                break;
            case LED_CONNECTING:                  // oranje knipperend
                on = !on;
                set_rgb(on ? 45 : 0, on ? 18 : 0, 0);
                vTaskDelay(pdMS_TO_TICKS(250));
                break;
            case LED_OFFLINE:                      // rood knipperend
                on = !on;
                set_rgb(on ? 45 : 0, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(250));
                break;
        }
    }
}

void status_led_init(void) {
    led_strip_config_t scfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false },
    };
    led_strip_rmt_config_t rcfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,   // 10 MHz
        .mem_block_symbols = 64,
        .flags = { .with_dma = false },
    };
    if (led_strip_new_rmt_device(&scfg, &rcfg, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "WS2812 niet geinitialiseerd (klopt GPIO8 voor jouw board?)");
        s_strip = NULL;
        return;
    }
    led_strip_clear(s_strip);
    xTaskCreate(led_task, "status_led", 2560, NULL, 3, NULL);
}

void status_led_set(led_state_t state) {
    s_state = state;
}

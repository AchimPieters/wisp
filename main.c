#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "dhcpserver/dhcpserver_options.h"

#include "config_store.h"
#include "portal.h"
#include "status_led.h"

static const char *TAG = "bridge";

static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;

#define RESET_GPIO  9      // BOOT-knop op de ESP32-C6 (Super Mini). C3/C6 = GPIO9

// Zet NAPT aan en geef de AP-clients een werkende DNS-server.
static void enable_internet_sharing(void) {
    // 1) DNS van het upstream-netwerk doorgeven aan onze DHCP-clients
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        dhcps_offer_t offer = OFFER_DNS;
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
        esp_netif_dhcps_start(ap_netif);
    }

    // 2) NAT/routing tussen AP-clients en de upstream-uplink
    esp_err_t e = esp_netif_napt_enable(ap_netif);
    ESP_LOGI(TAG, "NAPT: %s", esp_err_to_name(e));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Upstream weg, opnieuw verbinden...");
        status_led_set(LED_OFFLINE);
        esp_wifi_connect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Upstream verbonden -> internet delen aanzetten");
        status_led_set(LED_ONLINE);
        enable_internet_sharing();
    }
}

// BOOT-knop kort na het opstarten ingedrukt -> configuratie wissen.
static void maybe_reset_config(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    bool held = true;
    for (int i = 0; i < 15; i++) {            // ~1,5 s vasthouden
        if (gpio_get_level(RESET_GPIO)) { held = false; break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (held) {
        ESP_LOGW(TAG, "Reset-knop ingedrukt -> config gewist");
        config_erase();
    }
}

static void wifi_common_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
}

static void set_ap_config(const char *ssid, const char *pass) {
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;   // volgt automatisch het STA-kanaal zodra verbonden
    if (pass && strlen(pass) >= 8) {
        strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
}

static void set_sta_config(const char *ssid, const char *pass) {
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
}

void app_main(void) {
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    maybe_reset_config();
    status_led_init();
    wifi_common_init();

    app_config_t cfg;
    if (config_load(&cfg)) {
        // ---- Operationele modus ----
        status_led_set(LED_CONNECTING);
        set_sta_config(cfg.up_ssid, cfg.up_pass);
        set_ap_config(cfg.ap_ssid, cfg.ap_pass);
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_connect();
        status_server_start(cfg.ap_ssid);
        ESP_LOGI(TAG, "Operationeel: AP '%s' <- uplink '%s'", cfg.ap_ssid, cfg.up_ssid);
    } else {
        // ---- Setup-modus (captive portal) ----
        status_led_set(LED_SETUP);
        set_ap_config("Wisp-Setup", "wispsetup");
        ESP_ERROR_CHECK(esp_wifi_start());   // STA blijft idle, alleen voor scannen
        portal_start();
        ESP_LOGI(TAG, "Setup: verbind met 'Wisp-Setup' (wachtwoord wispsetup), ga naar 192.168.4.1");
    }
}

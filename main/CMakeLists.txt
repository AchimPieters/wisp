idf_component_register(
    SRCS "main.c" "config_store.c" "portal.c" "status_led.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi wpa_supplicant esp_netif esp_event esp_http_server lwip driver esp_system esp_timer led_strip
)

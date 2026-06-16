#pragma once
#include <stdbool.h>

#define CFG_SSID_MAX 33   // 32 tekens + null
#define CFG_PASS_MAX 65   // 64 tekens + null

typedef struct {
    char up_ssid[CFG_SSID_MAX];   // upstream WiFi (waar de ESP mee verbindt)
    char up_pass[CFG_PASS_MAX];
    char ap_ssid[CFG_SSID_MAX];   // eigen AP (waar Pi/laptop mee verbinden)
    char ap_pass[CFG_PASS_MAX];
} app_config_t;

// true als er een geldige configuratie in NVS staat
bool config_load(app_config_t *cfg);
bool config_save(const app_config_t *cfg);
void config_erase(void);

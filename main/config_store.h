#pragma once
#include <stdbool.h>

#define CFG_SSID_MAX 33   // 32 chars + null
#define CFG_PASS_MAX 65   // 64 chars + null
#define CFG_USER_MAX 65   // 64 chars + null (WPA2-Enterprise username)

typedef struct {
    char up_ssid[CFG_SSID_MAX];   // upstream WiFi (the network the ESP joins)
    char up_user[CFG_USER_MAX];   // username: empty = WPA2-PSK, set = WPA2-Enterprise (PEAP/MSCHAPv2)
    char up_pass[CFG_PASS_MAX];
    char ap_ssid[CFG_SSID_MAX];   // own AP (the network your Pi/laptop join)
    char ap_pass[CFG_PASS_MAX];
} app_config_t;

// true if a valid configuration is stored in NVS
bool config_load(app_config_t *cfg);
bool config_save(const app_config_t *cfg);
void config_erase(void);

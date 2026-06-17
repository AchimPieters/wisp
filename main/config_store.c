#include "config_store.h"
#include <string.h>
#include "nvs.h"

static const char *NS = "wifibridge";

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t outlen) {
    size_t len = outlen;
    if (nvs_get_str(h, key, out, &len) == ESP_OK) {
        return true;
    }
    out[0] = 0;
    return false;
}

bool config_load(app_config_t *cfg) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    memset(cfg, 0, sizeof(*cfg));
    bool have_up = get_str(h, "up_ssid", cfg->up_ssid, sizeof(cfg->up_ssid));
    get_str(h, "up_user", cfg->up_user, sizeof(cfg->up_user));   // empty = PSK, set = Enterprise
    get_str(h, "up_pass", cfg->up_pass, sizeof(cfg->up_pass));   // may be empty (open network)
    bool have_ap = get_str(h, "ap_ssid", cfg->ap_ssid, sizeof(cfg->ap_ssid));
    get_str(h, "ap_pass", cfg->ap_pass, sizeof(cfg->ap_pass));
    nvs_close(h);

    return have_up && have_ap &&
           strlen(cfg->up_ssid) > 0 && strlen(cfg->ap_ssid) > 0;
}

bool config_save(const app_config_t *cfg) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_str(h, "up_ssid", cfg->up_ssid);
    nvs_set_str(h, "up_user", cfg->up_user);
    nvs_set_str(h, "up_pass", cfg->up_pass);
    nvs_set_str(h, "ap_ssid", cfg->ap_ssid);
    nvs_set_str(h, "ap_pass", cfg->ap_pass);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void config_erase(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

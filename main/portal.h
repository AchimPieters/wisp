#pragma once

// Captive portal + DNS hijack (setup mode): lets the user pick an upstream
// network and configure the device's own access point.
void portal_start(void);

// Live status / reset page (operational mode).
void status_server_start(const char *ap_ssid);

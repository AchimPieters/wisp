#pragma once

// Captive portal + DNS-hijack (setup-modus): laat de gebruiker een
// upstream-netwerk kiezen en het eigen AP instellen.
void portal_start(void);

// Minimale status-/reset-pagina (operationele modus).
void status_server_start(const char *ap_ssid);

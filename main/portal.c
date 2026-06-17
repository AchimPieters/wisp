#include "portal.h"
#include "config_store.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netif.h"

static const char *TAG = "portal";

/* ----------------------------------------------------------------------
 *  DNS hijack: answers every DNS query with 192.168.4.1 so the operating
 *  system opens the captive portal automatically.
 *  Only active in setup mode!
 * -------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint16_t id, flags, qd, an, ns, ar;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t ptr;       // 0xC00C -> points to the name at offset 12
    uint16_t type;      // A
    uint16_t cls;       // IN
    uint32_t ttl;
    uint16_t rdlength;  // 4
    uint32_t rdata;     // the IP address
} dns_answer_t;

static void dns_task(void *pv) {
    uint8_t buf[512];
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { vTaskDelete(NULL); return; }

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    const uint32_t ap_ip = htonl(0xC0A80401); // 192.168.4.1

    while (1) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf) - sizeof(dns_answer_t), 0,
                           (struct sockaddr *)&src, &sl);
        if (len < (int)sizeof(dns_header_t) + 1) continue;

        dns_header_t *hdr = (dns_header_t *)buf;
        if (ntohs(hdr->flags) & 0x8000) continue;   // already a response

        // find the end of the first question (labels -> 0x00, then qtype+qclass)
        int p = sizeof(dns_header_t);
        while (p < len && buf[p] != 0) p += buf[p] + 1;
        p += 1 + 4;
        if (p > len) continue;

        hdr->flags = htons(0x8180);   // response, recursion available
        hdr->an = htons(1);
        hdr->ns = 0;
        hdr->ar = 0;

        dns_answer_t ans = {
            .ptr = htons(0xC00C),
            .type = htons(1),
            .cls = htons(1),
            .ttl = htonl(60),
            .rdlength = htons(4),
            .rdata = ap_ip,
        };
        memcpy(buf + p, &ans, sizeof(ans));
        sendto(sock, buf, p + sizeof(ans), 0, (struct sockaddr *)&src, sl);
    }
}

/* ----------------------------------------------------------------------
 *  Helpers: url-decode + extract a field from a form body
 * -------------------------------------------------------------------- */
static void urldecode(const char *src, size_t srclen, char *dst, size_t dstlen) {
    size_t di = 0;
    for (size_t i = 0; i < srclen && di + 1 < dstlen; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < srclen) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            c = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        dst[di++] = c;
    }
    dst[di] = 0;
}

// Walks the form body token by token (separated by '&') and compares the
// full field name, so 'pass' does not match 'up_pass'.
static bool form_get(const char *body, const char *key, char *out, size_t outlen) {
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', end - p);
        if (eq && (size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            urldecode(eq + 1, end - (eq + 1), out, outlen);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0;
    return false;
}

// Escapes a string for inclusion inside a JSON string (SSIDs are fully
// third-party controlled and may contain ", \ or control characters).
static void json_escape(const char *src, char *dst, size_t dstlen) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 7 < dstlen; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c < 0x20) {
            di += snprintf(dst + di, dstlen - di, "\\u%04x", c);
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = 0;
}

/* ----------------------------------------------------------------------
 *  HTTP handlers (setup mode)
 * -------------------------------------------------------------------- */
static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Wisp</title><style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#e9e9ee;"
    "margin:0;padding:22px 12px;color:#1c1c1e}"
    ".card-main{max-width:430px;margin:0 auto;background:#fff;border-radius:20px;padding:26px 22px;"
    "box-shadow:0 8px 30px rgba(0,0,0,.10)}"
    "h1{font-size:25px;font-weight:800;text-align:center;margin:4px 0 22px}"
    ".btn{display:block;width:100%;box-sizing:border-box;padding:14px;border:0;border-radius:13px;"
    "font-size:16px;font-weight:600;cursor:pointer}"
    ".btn-blue{background:#007aff;color:#fff}.btn-gray{background:#f2f2f7;color:#1c1c1e;margin-top:14px}"
    ".btn-green{background:#34c759;color:#fff;margin-top:22px;padding:16px}"
    ".nets{margin-top:16px;display:flex;flex-direction:column;gap:8px}"
    ".netrow{display:flex;align-items:center;justify-content:space-between;background:#f2f2f7;"
    "border:2px solid transparent;border-radius:12px;padding:14px 16px;cursor:pointer}"
    ".netrow.active{border-color:#007aff;background:#eaf3ff}"
    ".netrow .name{flex:1;margin-left:12px;font-size:16px}"
    "label{display:block;margin:18px 0 6px;font-weight:600}.req{color:#ff3b30}"
    "input{width:100%;box-sizing:border-box;padding:13px;border:1px solid #d1d1d6;border-radius:11px;"
    "font-size:16px;background:#fff}"
    ".sect{background:#f2f2f7;border-radius:15px;padding:20px;margin-top:22px}"
    ".sect h2{font-size:18px;font-weight:800;margin:0 0 6px}"
    ".foot{text-align:center;color:#8e8e93;font-size:12px;margin-top:22px}svg{flex:none}"
    "</style></head><body>";

static esp_err_t root_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<div class='card-main'>"
        "<h1>Choose a WiFi network</h1>"
        "<form action='/save' method='POST' onsubmit='return chk()'>"
        "<input type='hidden' id='up_ssid' name='up_ssid'>"
        "<button type='button' class='btn btn-blue' onclick='scan()'>&#8635; Refresh</button>"
        "<div class='nets' id='nets'></div>"
        "<button type='button' class='btn btn-gray' onclick='man()'>Choose another network</button>"
        "<input id='manin' placeholder='Network name' style='display:none;margin-top:12px' "
          "oninput=\"document.getElementById('up_ssid').value=this.value\">"
        "<label>Username <span style='color:#8e8e93;font-weight:400'>(WPA2-Enterprise only)</span></label>"
        "<input name='up_user' maxlength='64' autocapitalize='none' autocomplete='username'>"
        "<label><span class='req'>*</span> Password</label>"
        "<input name='up_pass' type='password' maxlength='64'>"
        "<div class='sect'>"
          "<h2>Your own network (Access Point)</h2>"
          "<label>Name</label>"
          "<input name='ap_ssid' maxlength='32' required>"
          "<label>Password (min. 8 characters)</label>"
          "<input name='ap_pass' type='password' maxlength='64' minlength='8'>"
        "</div>"
        "<button type='submit' class='btn btn-green'>Save and connect</button>"
        "</form>"
        "<div class='foot'>Wisp</div>"
        "</div>"
        "<script>"
        "var W=\"<svg viewBox='0 0 24 24' width='22' height='22' fill='#8e8e93'><path d='M12 18a2 2 0 100 4 2 2 0 000-4zM1.3 7.3l2.1 2.1C5.6 7.2 8.6 6 12 6s6.4 1.2 8.6 3.4l2.1-2.1C19.9 4.6 16.1 3 12 3 7.9 3 4.1 4.6 1.3 7.3zm4.9 4.9l2.1 2.1C8.4 11.7 10.1 11 12 11s3.6.7 4.9 1.9l2.1-2.1C17.2 9.1 14.7 8 12 8s-5 1.1-6.8 2.8z'/></svg>\";"
        "var L=\"<svg viewBox='0 0 24 24' width='17' height='17' fill='#8e8e93'><path d='M12 1a5 5 0 00-5 5v3H6a2 2 0 00-2 2v9a2 2 0 002 2h12a2 2 0 002-2v-9a2 2 0 00-2-2h-1V6a5 5 0 00-5-5zm3 8H9V6a3 3 0 016 0v3z'/></svg>\";"
        "function esc(s){return s.replace(/[<>&]/g,function(c){return '&#'+c.charCodeAt(0)+';';});}"
        "function scan(){var n=document.getElementById('nets');"
        "n.innerHTML='<div style=\"color:#8e8e93;padding:6px\">Scanning...</div>';"
        "fetch('/scan').then(function(r){return r.json();}).then(function(l){n.innerHTML='';"
        "l.forEach(function(x){var d=document.createElement('div');d.className='netrow';"
        "d.innerHTML=W+'<span class=\"name\">'+esc(x.ssid)+'</span>'+(x.lock?L:'<span></span>');"
        "d.onclick=function(){sel(x.ssid,d);};n.appendChild(d);});});}"
        "function sel(s,d){document.getElementById('up_ssid').value=s;"
        "var r=document.querySelectorAll('.netrow');for(var i=0;i<r.length;i++)r[i].classList.remove('active');"
        "d.classList.add('active');document.getElementById('manin').style.display='none';}"
        "function man(){var m=document.getElementById('manin');m.style.display='block';m.focus();}"
        "function chk(){if(!document.getElementById('up_ssid').value){"
        "alert('Pick or type an upstream network');return false;}return true;}"
        "scan();</script>");
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t scan_get(httpd_req_t *req) {
    wifi_scan_config_t sc = { 0 };
    esp_wifi_scan_start(&sc, true);   // blocking

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;
    static wifi_ap_record_t recs[20];   // static: saves ~1.5 kB of httpd stack
    esp_wifi_scan_get_ap_records(&num, recs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    char line[160];
    char ssid[2 * CFG_SSID_MAX + 8];
    bool first = true;
    for (int i = 0; i < num; i++) {
        if (recs[i].ssid[0] == 0) continue;   // hide hidden SSIDs
        json_escape((char *)recs[i].ssid, ssid, sizeof(ssid));
        snprintf(line, sizeof(line),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"lock\":%s}",
                 first ? "" : ",", ssid, recs[i].rssi,
                 recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        first = false;
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *req) {
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) return ESP_FAIL;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[total] = 0;

    app_config_t cfg = { 0 };
    form_get(body, "up_ssid", cfg.up_ssid, sizeof(cfg.up_ssid));
    form_get(body, "up_user", cfg.up_user, sizeof(cfg.up_user));
    form_get(body, "up_pass", cfg.up_pass, sizeof(cfg.up_pass));
    form_get(body, "ap_ssid", cfg.ap_ssid, sizeof(cfg.ap_ssid));
    form_get(body, "ap_pass", cfg.ap_pass, sizeof(cfg.ap_pass));
    free(body);

    bool ok = strlen(cfg.up_ssid) > 0 && strlen(cfg.ap_ssid) > 0 &&
              (strlen(cfg.ap_pass) == 0 || strlen(cfg.ap_pass) >= 8) &&
              // WPA2-Enterprise (username filled in) requires a password.
              (strlen(cfg.up_user) == 0 || strlen(cfg.up_pass) > 0);

    httpd_resp_set_type(req, "text/html");
    if (!ok) {
        httpd_resp_sendstr_chunk(req, PAGE_HEAD);
        httpd_resp_sendstr_chunk(req,
            "<div class='card-main'><h1>Invalid input</h1>"
            "<p>Upstream SSID and AP name are required; the AP password must be empty or at least 8 characters. "
            "When a username is given (WPA2-Enterprise) a password is required.</p>"
            "<p><a href=/>Back</a></p></div></body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    config_save(&cfg);
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<div class='card-main'><h1>Saved</h1>"
        "<p>The device restarts and connects to the network. "
        "After that, connect your Raspberry Pi and laptop to your own AP.</p></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

// Catches all other requests and redirects to the portal (captive trigger).
static esp_err_t redirect_get(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void portal_start(void) {
    xTaskCreate(dns_task, "dns_hijack", 3072, NULL, 5, NULL);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.max_uri_handlers = 8;
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &hc) != ESP_OK) return;

    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = root_get };
    httpd_uri_t scan = { .uri = "/scan", .method = HTTP_GET,  .handler = scan_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t any  = { .uri = "/*",    .method = HTTP_GET,  .handler = redirect_get };
    httpd_register_uri_handler(s, &root);
    httpd_register_uri_handler(s, &scan);
    httpd_register_uri_handler(s, &save);
    httpd_register_uri_handler(s, &any);   // last = catch-all
}

/* ----------------------------------------------------------------------
 *  Status / reset server (operational mode)
 * -------------------------------------------------------------------- */
static char s_ap_ssid[CFG_SSID_MAX];
static char s_csrf[17];   // 16 hex chars + null, regenerated every boot

// Only clients on our own AP subnet (192.168.4.0/24) may make changes, so
// /reset is not reachable from the upstream network.
static bool req_from_ap(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return false;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return false;
    if (ss.ss_family != AF_INET) return false;
    uint32_t ip = ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
    return (ip & 0xFFFFFF00) == 0xC0A80400;   // 192.168.4.0/24
}

// Count received bytes per interface without recompiling lwip: we replace the
// lwip netif 'input' function once with a wrapper that adds up p->tot_len and
// then calls the original. STA-in = download (from the internet),
// AP-in = upload (from the clients). idx 0 = STA, 1 = AP.
static netif_input_fn s_orig_input[2];
static volatile uint32_t s_rx_bytes[2];
static struct netif *s_hooked[2];

static err_t rx_hook_sta(struct pbuf *p, struct netif *inp) {
    s_rx_bytes[0] += p->tot_len;
    return s_orig_input[0](p, inp);
}
static err_t rx_hook_ap(struct pbuf *p, struct netif *inp) {
    s_rx_bytes[1] += p->tot_len;
    return s_orig_input[1](p, inp);
}

// Installs the counter hook once (as soon as the netif exists) and returns
// the current cumulative byte count.
static uint32_t netif_rx_bytes(int idx, const char *ifkey, netif_input_fn hook) {
    if (!s_hooked[idx]) {
        esp_netif_t *n = esp_netif_get_handle_from_ifkey(ifkey);
        if (n) {
            struct netif *impl = esp_netif_get_netif_impl(n);
            if (impl && impl->input != hook) {
                s_orig_input[idx] = impl->input;
                impl->input = hook;
                s_hooked[idx] = impl;
            }
        }
    }
    return s_rx_bytes[idx];
}

static const char *sta_phy_mode(const wifi_sta_info_t *s) {
    if (s->phy_11ax) return "11ax";
    if (s->phy_11n)  return "11n";
    if (s->phy_11g)  return "11g";
    if (s->phy_11b)  return "11b";
    if (s->phy_lr)   return "LR";
    return "?";
}

// JSON with all live data; polled periodically by the status page.
static esp_err_t info_get(httpd_req_t *req) {
    wifi_sta_list_t sl = { 0 };
    esp_wifi_ap_get_sta_list(&sl);
    wifi_sta_mac_ip_list_t ipl = { 0 };
    esp_wifi_ap_get_sta_list_with_ip(&sl, &ipl);

    wifi_ap_record_t ap;
    bool up = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    esp_netif_ip_info_t sta_ip = { 0 }, ap_ip = { 0 };
    esp_netif_t *stah = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_t *aph  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (stah) esp_netif_get_ip_info(stah, &sta_ip);
    if (aph)  esp_netif_get_ip_info(aph,  &ap_ip);

    char ap_ssid_esc[2 * CFG_SSID_MAX + 8];
    char up_ssid_esc[2 * 32 + 8];
    json_escape(s_ap_ssid, ap_ssid_esc, sizeof(ap_ssid_esc));
    json_escape(up ? (char *)ap.ssid : "", up_ssid_esc, sizeof(up_ssid_esc));

    char ipbuf[16], gwbuf[16], apbuf[16];
    esp_ip4addr_ntoa(&sta_ip.ip, ipbuf, sizeof(ipbuf));
    esp_ip4addr_ntoa(&sta_ip.gw, gwbuf, sizeof(gwbuf));
    esp_ip4addr_ntoa(&ap_ip.ip,  apbuf, sizeof(apbuf));

    httpd_resp_set_type(req, "application/json");
    char line[320];
    snprintf(line, sizeof(line),
        "{\"ap_ssid\":\"%s\",\"up\":%s,\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"sta_ip\":\"%s\",\"gw\":\"%s\",\"ap_ip\":\"%s\",",
        ap_ssid_esc, up ? "true" : "false", up_ssid_esc,
        up ? ap.rssi : 0, up ? ap.primary : 0, ipbuf, gwbuf, apbuf);
    httpd_resp_sendstr_chunk(req, line);

    snprintf(line, sizeof(line),
        "\"down_bytes\":%lu,\"up_bytes\":%lu,\"t\":%lld,\"heap\":%lu,\"uptime\":%lld,",
        (unsigned long)netif_rx_bytes(0, "WIFI_STA_DEF", rx_hook_sta),
        (unsigned long)netif_rx_bytes(1, "WIFI_AP_DEF",  rx_hook_ap),
        (long long)(esp_timer_get_time() / 1000),
        (unsigned long)esp_get_free_heap_size(),
        (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_sendstr_chunk(req, line);

    httpd_resp_sendstr_chunk(req, "\"clients\":[");
    for (int i = 0; i < sl.num; i++) {
        const uint8_t *m = sl.sta[i].mac;
        char macbuf[18], cipbuf[16];
        snprintf(macbuf, sizeof(macbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        esp_ip4addr_ntoa(&ipl.sta[i].ip, cipbuf, sizeof(cipbuf));
        snprintf(line, sizeof(line),
            "%s{\"mac\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"mode\":\"%s\"}",
            i ? "," : "", macbuf, cipbuf, sl.sta[i].rssi, sta_phy_mode(&sl.sta[i]));
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t status_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<div class='card-main'>"
        "<h1>WiFi bridge active</h1>"
        "<p>Own network: <b id='apn'>\xe2\x80\xa6</b></p>"
        "<p id='up'>\xe2\x80\xa6</p>"
        "<p id='ips' style='color:#8e8e93;font-size:13px;word-break:break-all'></p>"
        "<div class='sect'><h2>Speed</h2>"
        "<div style='display:flex;gap:12px;text-align:center'>"
        "<div style='flex:1'>&#8595; Download<br><b id='dn' style='font-size:22px'>\xe2\x80\x93</b></div>"
        "<div style='flex:1'>&#8593; Upload<br><b id='ul' style='font-size:22px'>\xe2\x80\x93</b></div>"
        "</div></div>"
        "<div class='sect'><h2>Connected devices (<span id='cnt'>0</span>)</h2>"
        "<table style='width:100%;border-collapse:collapse;font-size:13px'>"
        "<thead><tr style='text-align:left;color:#8e8e93'>"
        "<th>IP</th><th>MAC</th><th>Signal</th><th>Mode</th></tr></thead>"
        "<tbody id='rows'></tbody></table></div>"
        "<p class='foot' id='sys'></p>");
    char form[260];
    snprintf(form, sizeof(form),
        "<form action=/reset method=post>"
        "<input type=hidden name=csrf value=\"%s\">"
        "<button type=submit class='btn btn-gray'>Reconfigure (erase config)</button>"
        "</form></div>",
        s_csrf);
    httpd_resp_sendstr_chunk(req, form);
    httpd_resp_sendstr_chunk(req,
        "<script>"
        "function esc(s){return (s||'').replace(/[<>&]/g,function(c){return '&#'+c.charCodeAt(0)+';';});}"
        "function fmt(b){var u=['B/s','KB/s','MB/s'];var i=0;while(b>=1024&&i<2){b/=1024;i++;}"
        "return (i&&b<10?b.toFixed(1):Math.round(b))+' '+u[i];}"
        "var pT=null,pD=0,pU=0;"
        "function tick(){fetch('/info').then(function(r){return r.json();}).then(function(d){"
        "document.getElementById('apn').textContent=d.ap_ssid;"
        "document.getElementById('cnt').textContent=d.clients.length;"
        "document.getElementById('up').innerHTML=d.up?('Upstream: <b>'+esc(d.ssid)+'</b> &middot; channel '+d.ch+' &middot; '+d.rssi+' dBm'):'Upstream: <b>not connected</b>';"
        "document.getElementById('ips').innerHTML='Bridge IP '+d.ap_ip+' &middot; WAN IP '+d.sta_ip+' &middot; gateway '+d.gw;"
        "if(pT!==null){var dt=(d.t-pT)/1000;if(dt>0){"
        "var dd=d.down_bytes-pD;if(dd<0)dd+=4294967296;"
        "var du=d.up_bytes-pU;if(du<0)du+=4294967296;"
        "document.getElementById('dn').textContent=fmt(dd/dt);"
        "document.getElementById('ul').textContent=fmt(du/dt);}}"
        "pT=d.t;pD=d.down_bytes;pU=d.up_bytes;"
        "var h='';d.clients.forEach(function(c){h+='<tr><td>'+c.ip+'</td>"
        "<td style=\"font-family:monospace\">'+c.mac+'</td><td>'+c.rssi+' dBm</td><td>'+c.mode+'</td></tr>';});"
        "document.getElementById('rows').innerHTML=h||'<tr><td colspan=4 style=\"color:#8e8e93\">No devices</td></tr>';"
        "document.getElementById('sys').textContent='Uptime '+d.uptime+' s &middot; free heap '+Math.round(d.heap/1024)+' KB';"
        "}).catch(function(){});}"
        "tick();setInterval(tick,2000);"
        "</script></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t reset_post(httpd_req_t *req) {
    if (!req_from_ap(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "only via the device's own AP");
        return ESP_FAIL;
    }
    int total = req->content_len;
    if (total <= 0 || total > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    char buf[257];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    buf[total] = 0;

    char tok[sizeof(s_csrf)];
    form_get(buf, "csrf", tok, sizeof(tok));
    if (s_csrf[0] == 0 || strcmp(tok, s_csrf) != 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "invalid token");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Web reset: erasing config and restarting");
    config_erase();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<div class='card-main'><h1>Erased</h1>"
        "<p>The device restarts in setup mode.</p></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

void status_server_start(const char *ap_ssid) {
    strlcpy(s_ap_ssid, ap_ssid ? ap_ssid : "", sizeof(s_ap_ssid));
    snprintf(s_csrf, sizeof(s_csrf), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &hc) != ESP_OK) return;
    httpd_uri_t root = { .uri = "/",      .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t info = { .uri = "/info",  .method = HTTP_GET,  .handler = info_get };
    httpd_uri_t rst  = { .uri = "/reset", .method = HTTP_POST, .handler = reset_post };
    httpd_register_uri_handler(s, &root);
    httpd_register_uri_handler(s, &info);
    httpd_register_uri_handler(s, &rst);
}

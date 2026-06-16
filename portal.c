#include "portal.h"
#include "config_store.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "portal";

/* ----------------------------------------------------------------------
 *  DNS-hijack: beantwoordt elke DNS-vraag met 192.168.4.1, zodat het
 *  besturingssysteem het captive portal automatisch opent.
 *  Alleen actief in setup-modus!
 * -------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint16_t id, flags, qd, an, ns, ar;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t ptr;       // 0xC00C -> verwijst naar de naam op offset 12
    uint16_t type;      // A
    uint16_t cls;       // IN
    uint32_t ttl;
    uint16_t rdlength;  // 4
    uint32_t rdata;     // het IP-adres
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
        ESP_LOGE(TAG, "DNS bind mislukt");
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
        if (ntohs(hdr->flags) & 0x8000) continue;   // is al een antwoord

        // einde van de eerste vraag zoeken (labels -> 0x00, dan qtype+qclass)
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
 *  Hulpfuncties: url-decode + veld uit een form-body halen
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

static bool form_get(const char *body, const char *key, char *out, size_t outlen) {
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        bool boundary = (p == body) || (*(p - 1) == '&');
        if (boundary && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            urldecode(v, n, out, outlen);
            return true;
        }
        p += klen;
    }
    out[0] = 0;
    return false;
}

/* ----------------------------------------------------------------------
 *  HTTP-handlers (setup-modus)
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
        "<h1>Kies een WiFi-netwerk</h1>"
        "<form action='/save' method='POST' onsubmit='return chk()'>"
        "<input type='hidden' id='up_ssid' name='up_ssid'>"
        "<button type='button' class='btn btn-blue' onclick='scan()'>&#8635; Vernieuwen</button>"
        "<div class='nets' id='nets'></div>"
        "<button type='button' class='btn btn-gray' onclick='man()'>Ander netwerk kiezen</button>"
        "<input id='manin' placeholder='Netwerknaam' style='display:none;margin-top:12px' "
          "oninput=\"document.getElementById('up_ssid').value=this.value\">"
        "<label><span class='req'>*</span> Wachtwoord</label>"
        "<input name='up_pass' type='password' maxlength='64'>"
        "<div class='sect'>"
          "<h2>Eigen netwerk (Access Point)</h2>"
          "<label>Naam</label>"
          "<input name='ap_ssid' maxlength='32' required>"
          "<label>Wachtwoord (min. 8 tekens)</label>"
          "<input name='ap_pass' type='password' maxlength='64' minlength='8'>"
        "</div>"
        "<button type='submit' class='btn btn-green'>Opslaan en verbinden</button>"
        "</form>"
        "<div class='foot'>Wisp</div>"
        "</div>"
        "<script>"
        "var W=\"<svg viewBox='0 0 24 24' width='22' height='22' fill='#8e8e93'><path d='M12 18a2 2 0 100 4 2 2 0 000-4zM1.3 7.3l2.1 2.1C5.6 7.2 8.6 6 12 6s6.4 1.2 8.6 3.4l2.1-2.1C19.9 4.6 16.1 3 12 3 7.9 3 4.1 4.6 1.3 7.3zm4.9 4.9l2.1 2.1C8.4 11.7 10.1 11 12 11s3.6.7 4.9 1.9l2.1-2.1C17.2 9.1 14.7 8 12 8s-5 1.1-6.8 2.8z'/></svg>\";"
        "var L=\"<svg viewBox='0 0 24 24' width='17' height='17' fill='#8e8e93'><path d='M12 1a5 5 0 00-5 5v3H6a2 2 0 00-2 2v9a2 2 0 002 2h12a2 2 0 002-2v-9a2 2 0 00-2-2h-1V6a5 5 0 00-5-5zm3 8H9V6a3 3 0 016 0v3z'/></svg>\";"
        "function esc(s){return s.replace(/[<>&]/g,function(c){return '&#'+c.charCodeAt(0)+';';});}"
        "function scan(){var n=document.getElementById('nets');"
        "n.innerHTML='<div style=\"color:#8e8e93;padding:6px\">Scannen...</div>';"
        "fetch('/scan').then(function(r){return r.json();}).then(function(l){n.innerHTML='';"
        "l.forEach(function(x){var d=document.createElement('div');d.className='netrow';"
        "d.innerHTML=W+'<span class=\"name\">'+esc(x.ssid)+'</span>'+(x.lock?L:'<span></span>');"
        "d.onclick=function(){sel(x.ssid,d);};n.appendChild(d);});});}"
        "function sel(s,d){document.getElementById('up_ssid').value=s;"
        "var r=document.querySelectorAll('.netrow');for(var i=0;i<r.length;i++)r[i].classList.remove('active');"
        "d.classList.add('active');document.getElementById('manin').style.display='none';}"
        "function man(){var m=document.getElementById('manin');m.style.display='block';m.focus();}"
        "function chk(){if(!document.getElementById('up_ssid').value){"
        "alert('Kies of typ een upstream-netwerk');return false;}return true;}"
        "scan();</script>");
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t scan_get(httpd_req_t *req) {
    wifi_scan_config_t sc = { 0 };
    esp_wifi_scan_start(&sc, true);   // blokkerend

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&num, recs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    char line[160];
    for (int i = 0; i < num; i++) {
        if (recs[i].ssid[0] == 0) continue;   // verberg hidden SSIDs
        snprintf(line, sizeof(line),
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"lock\":%s}",
                 i ? "," : "", (char *)recs[i].ssid, recs[i].rssi,
                 recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
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
    form_get(body, "up_pass", cfg.up_pass, sizeof(cfg.up_pass));
    form_get(body, "ap_ssid", cfg.ap_ssid, sizeof(cfg.ap_ssid));
    form_get(body, "ap_pass", cfg.ap_pass, sizeof(cfg.ap_pass));
    free(body);

    bool ok = strlen(cfg.up_ssid) > 0 && strlen(cfg.ap_ssid) > 0 &&
              (strlen(cfg.ap_pass) == 0 || strlen(cfg.ap_pass) >= 8);

    httpd_resp_set_type(req, "text/html");
    if (!ok) {
        httpd_resp_sendstr_chunk(req, PAGE_HEAD);
        httpd_resp_sendstr_chunk(req,
            "<div class='card-main'><h1>Ongeldige invoer</h1>"
            "<p>Upstream-SSID en AP-naam zijn verplicht; AP-wachtwoord leeg of minstens 8 tekens.</p>"
            "<p><a href=/>Terug</a></p></div></body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    config_save(&cfg);
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<div class='card-main'><h1>Opgeslagen</h1>"
        "<p>De ESP herstart en verbindt met het netwerk. "
        "Verbind daarna je Raspberry Pi en laptop met je eigen AP.</p></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

// Vangt alle overige verzoeken op en stuurt naar het portaal (captive trigger)
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
    httpd_register_uri_handler(s, &any);   // laatste = catch-all
}

/* ----------------------------------------------------------------------
 *  Status-/reset-server (operationele modus)
 * -------------------------------------------------------------------- */
static char s_ap_ssid[CFG_SSID_MAX];

static int ap_station_count(void) {
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) return list.num;
    return 0;
}

static esp_err_t status_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    char body[360];
    snprintf(body, sizeof(body),
        "<div class='card-main'><h1>WiFi bridge actief</h1>"
        "<p>Eigen netwerk: <b>%s</b></p>"
        "<p>Verbonden apparaten: %d</p>"
        "<form action=/reset method=post>"
        "<button type=submit class='btn btn-gray'>Opnieuw instellen (config wissen)</button></form>"
        "</div></body></html>",
        s_ap_ssid, ap_station_count());
    httpd_resp_sendstr_chunk(req, body);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t reset_post(httpd_req_t *req) {
    config_erase();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req,
        "<div class='card-main'><h1>Gewist</h1>"
        "<p>De ESP herstart in setup-modus.</p></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

void status_server_start(const char *ap_ssid) {
    strlcpy(s_ap_ssid, ap_ssid ? ap_ssid : "", sizeof(s_ap_ssid));
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &hc) != ESP_OK) return;
    httpd_uri_t root = { .uri = "/",      .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t rst  = { .uri = "/reset", .method = HTTP_POST, .handler = reset_post };
    httpd_register_uri_handler(s, &root);
    httpd_register_uri_handler(s, &rst);
}

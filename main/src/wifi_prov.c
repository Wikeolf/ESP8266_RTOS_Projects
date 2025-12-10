#include "wifi_prov.h"
#include "utils.h"

#include <string.h>
#include <stdlib.h> // for malloc
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include <sys/param.h>

static const char *TAG = "WiFi_Prov";
static httpd_handle_t server = NULL;

// 最大重连次数
#define MAX_RETRY 5
// 当前重连计数
static int s_retry_num = 0;

/* * 前端页面 HTML 
 * 新增了 scanWifi() JS 函数和 scanBtn 按钮 
 * 优化: 信号强度 Emoji 显示 + BSSID 显示 + 自动填充 BSSID
 */
const char* index_html = 
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<meta charset=\"UTF-8\">"
    "<title>ESP8266 Config</title>"
    "<style>"
    "  body { font-family: sans-serif; padding: 20px; max-width: 400px; margin: 0 auto; }"
    "  input, select, button { padding: 10px; margin: 5px 0; width: 100%; box-sizing: border-box; }"
    "  button { background-color: #007bff; color: white; border: none; cursor: pointer; }"
    "  button:disabled { background-color: #ccc; }"
    "  .advanced { margin-top: 15px; padding: 10px; border: 1px solid #ccc; background: #f9f9f9; }"
    "  #scan_res { display: none; margin-top: 5px; }"
    "</style>"
    "<script>"
    "  function validateForm() {"
    "    var ssid = document.getElementById('ssid').value;"
    "    var pass = document.getElementById('password').value;"
    "    var bssidCheck = document.getElementById('bssid_enable').checked;"
    "    var bssid = document.getElementById('bssid').value;"
    "    if (ssid.length == 0) { alert('SSID cannot be empty'); return false; }"
    "    if (ssid.length > 32) { alert('SSID too long'); return false; }"
    "    if (pass.length > 63) { alert('Password too long'); return false; }"
    "    if (bssidCheck) {"
    "      var macRegex = /^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/;"
    "      if (!macRegex.test(bssid)) { alert('Invalid BSSID'); return false; }"
    "    }"
    "    return true;"
    "  }"
    "  function toggleAdvanced() {"
    "    var div = document.getElementById('bssid_div');"
    "    var check = document.getElementById('bssid_enable');"
    "    div.style.display = check.checked ? 'block' : 'none';"
    "  }"
    "  function getSignalEmoji(rssi) {"
    "    if (rssi >= -55) return '🟢';" // Excellent
    "    if (rssi >= -75) return '🟡';" // Good
    "    if (rssi >= -85) return '🟠';" // Fair
    "    return '🔴';"                 // Weak
    "  }"
    "  function scanWifi() {"
    "    var btn = document.getElementById('scanBtn');"
    "    var sel = document.getElementById('scan_res');"
    "    btn.disabled = true;"
    "    btn.innerText = 'Scanning...';"
    "    fetch('/scan').then(res => res.json()).then(data => {"
    "       sel.innerHTML = '<option value=\"\">-- Select Network --</option>';"
    "       data.forEach(ap => {"
    "         var opt = document.createElement('option');"
    "         opt.value = ap.ssid;"
    "         /* 将 BSSID 存入 dataset 以便选择时读取 */"
    "         opt.dataset.bssid = ap.bssid;" 
    "         var lock = ap.auth == 0 ? '' : '🔒';"
    "         var emoji = getSignalEmoji(ap.rssi);"
    "         /* 显示格式: 🟢 SSID (-50dBm) [MAC] 🔒 */"
    "         opt.innerText = emoji + ' ' + ap.ssid + ' (' + ap.rssi + 'dBm) [' + ap.bssid + '] ' + lock;"
    "         sel.appendChild(opt);"
    "       });"
    "       sel.style.display = 'block';"
    "       btn.disabled = false;"
    "       btn.innerText = 'Rescan';"
    "    }).catch(e => {"
    "       alert('Scan failed: ' + e);"
    "       btn.disabled = false;"
    "       btn.innerText = 'Scan WiFi';"
    "    });"
    "  }"
    "  function selectWifi() {"
    "     var sel = document.getElementById('scan_res');"
    "     var selectedOpt = sel.options[sel.selectedIndex];"
    "     if(sel.value) {"
    "       document.getElementById('ssid').value = sel.value;"
    "       /* 如果存在 BSSID 且用户开启了锁定功能(或为了方便用户查看)，尝试填充 */"
    "       if (selectedOpt.dataset.bssid) {"
    "          document.getElementById('bssid').value = selectedOpt.dataset.bssid;"
    "       }"
    "     }"
    "  }"
    "</script>"
    "</head>"
    "<body>"
    "<h2>WiFi Configuration</h2>"
    "<form action=\"/config\" method=\"post\" onsubmit=\"return validateForm()\">"
    
    "SSID:<br>"
    "<input type=\"text\" id=\"ssid\" name=\"ssid\" maxlength=\"32\" placeholder=\"Enter SSID\">"
    "<button type=\"button\" id=\"scanBtn\" onclick=\"scanWifi()\">Scan Networks</button>"
    "<select id=\"scan_res\" onchange=\"selectWifi()\"></select>"
    "<br>"
    
    "Password:<br><input type=\"text\" id=\"password\" name=\"password\" maxlength=\"63\" placeholder=\"Enter Password\"><br>"
    
    "<div class=\"advanced\">"
    "  <strong>Advanced Settings</strong><br>"
    "  <label><input type=\"checkbox\" id=\"bssid_enable\" name=\"bssid_enable\" onclick=\"toggleAdvanced()\"> Lock BSSID</label>"
    "  <div id=\"bssid_div\" style=\"display:none\">"
    "    BSSID (Auto-filled from scan):<br>"
    "    <input type=\"text\" id=\"bssid\" name=\"bssid\" maxlength=\"17\" placeholder=\"AA:BB:CC:DD:EE:FF\">"
    "  </div>"
    "</div>"
    
    "<br><input type=\"submit\" value=\"Connect\">"
    "</form>"
    "</body>"
    "</html>";

/* --- 内部函数声明 --- */
static void start_webserver();

/* WiFi 事件处理 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d", MAC2STR(event->mac), event->aid);
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0; // 成功连接，重置重试计数
        if (server) {
            ESP_LOGI(TAG, "Provisioning Successful! Restarting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // 自动重连逻辑
        if (s_retry_num < MAX_RETRY) {
            s_retry_num++;
            
            // 采用线性退避策略 (Linear Backoff)
            // 每次重试增加 1500ms 延迟
            // Retry 1: 1500ms
            // Retry 2: 3000ms
            // Retry 3: 4500ms ...
            int delay_ms = s_retry_num * 1500;
            ESP_LOGI(TAG, "Retry connect (%d/%d) after %d ms", s_retry_num, MAX_RETRY, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Connect failed. Max retries reached.");
        }
    }
}

/* HTTP GET Handler - 返回配网页面 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

/* * HTTP GET Handler - 执行 WiFi 扫描并返回 JSON 
 * 响应格式: [{"ssid":"ABC","rssi":-50,"auth":3,"bssid":"xx:xx..."}, ...]
 */
static esp_err_t scan_get_handler(httpd_req_t *req)
{
    // 配置扫描参数 (block=true 表示阻塞直到扫描完成)
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };
    
    // 开始扫描 (阻塞模式)
    ESP_LOGI(TAG, "Starting WiFi Scan...");
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed (0x%x)", err);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Found %d APs", ap_count);

    if (ap_count == 0) {
        httpd_resp_send(req, "[]", 2);
        return ESP_OK;
    }

    // 限制最大处理数量以防止内存溢出
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    // 构建 JSON 字符串
    // 估算缓冲区大小: 
    // 每个 AP 约: 
    // {"ssid":"...","rssi":-xx,"auth":x,"bssid":"xx:xx:xx:xx:xx:xx"}
    // SSID(32) + RSSI(5) + Auth(3) + BSSID(19) + Keys/Quotes(40) ~= 100 bytes
    // 安全起见给 128 bytes/AP
    char *json_buf = malloc(ap_count * 128 + 10); 
    if (!json_buf) {
        free(ap_list);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *ptr = json_buf;
    ptr += sprintf(ptr, "[");

    for (int i = 0; i < ap_count; i++) {
        // 过滤空 SSID
        if (strlen((char *)ap_list[i].ssid) == 0) continue;

        ptr += sprintf(ptr, "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}", 
                       ap_list[i].ssid, ap_list[i].rssi, ap_list[i].authmode,
                       ap_list[i].bssid[0], ap_list[i].bssid[1], ap_list[i].bssid[2],
                       ap_list[i].bssid[3], ap_list[i].bssid[4], ap_list[i].bssid[5]);
        
        if (i < ap_count - 1) {
            ptr += sprintf(ptr, ",");
        }
    }
    // 处理最后一个可能的逗号
    if (*(ptr - 1) == ',') {
        ptr--; 
    }

    ptr += sprintf(ptr, "]");
    
    // 发送响应
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buf, strlen(json_buf));

    free(json_buf);
    free(ap_list);
    return ESP_OK;
}

/* HTTP POST Handler - 保存配置 */
static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = malloc(total_len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    int received = 0;
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            free(buf);
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    char ssid_encoded[128] = {0}; 
    char password_encoded[128] = {0};
    char bssid_encoded[64] = {0};
    char bssid_enable[10] = {0};

    if (httpd_query_key_value(buf, "ssid", ssid_encoded, sizeof(ssid_encoded)) == ESP_OK &&
        httpd_query_key_value(buf, "password", password_encoded, sizeof(password_encoded)) == ESP_OK) {
        
        char ssid_decoded[33] = {0};
        char password_decoded[65] = {0};

        url_decode(ssid_decoded, ssid_encoded, sizeof(ssid_decoded));
        url_decode(password_decoded, password_encoded, sizeof(password_decoded));

        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, ssid_decoded, sizeof(wifi_config.sta.ssid)); 
        strncpy((char*)wifi_config.sta.password, password_decoded, sizeof(wifi_config.sta.password));
        
        if (httpd_query_key_value(buf, "bssid_enable", bssid_enable, sizeof(bssid_enable)) == ESP_OK &&
            strcmp(bssid_enable, "on") == 0) {
            
            if (httpd_query_key_value(buf, "bssid", bssid_encoded, sizeof(bssid_encoded)) == ESP_OK) {
                char bssid_decoded[20] = {0};
                url_decode(bssid_decoded, bssid_encoded, sizeof(bssid_decoded));
                if (parse_mac_address(bssid_decoded, wifi_config.sta.bssid)) {
                    wifi_config.sta.bssid_set = true;
                }
            }
        }
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_LOGI(TAG, "Connecting to router...");
        
        // 收到新配置时，重置重试计数器，给新密码 5 次机会
        s_retry_num = 0;
        esp_wifi_connect();
        
        const char *resp_str = "Connecting... Please check device status.";
        httpd_resp_send(req, resp_str, strlen(resp_str));
    } else {
        httpd_resp_send_500(req);
    }

    free(buf); 
    return ESP_OK;
}

/* 注册 URI */
static const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
static const httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler, .user_ctx = NULL };
static const httpd_uri_t config_uri = { .uri = "/config", .method = HTTP_POST, .handler = wifi_config_post_handler, .user_ctx = NULL };

static void start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    ESP_LOGI(TAG, "Starting webserver on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &scan_uri); // 注册扫描接口
        httpd_register_uri_handler(server, &config_uri);
    }
}

/* 公开的初始化函数 */
void wifi_init_softap_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config;
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));

    if (wifi_config.sta.ssid[0] != 0) {
        ESP_LOGI(TAG, "NVS config found (SSID: %s). Starting in STA Mode.", wifi_config.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else {
         ESP_LOGI(TAG, "No NVS config. Starting AP+STA for Provisioning.");
         wifi_config_t ap_config = {
             .ap = {
                 .ssid = AP_SSID,
                 .ssid_len = strlen(AP_SSID),
                 .password = AP_PASS,
                 .max_connection = 4,
                 .authmode = WIFI_AUTH_OPEN,
                 .channel = 1
             },
         };
         ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
         ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
         ESP_ERROR_CHECK(esp_wifi_start());
         start_webserver();
    }
}
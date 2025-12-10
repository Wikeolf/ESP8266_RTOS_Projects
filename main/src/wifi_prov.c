#include "wifi_prov.h"
#include "utils.h"

#include <string.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include <sys/param.h>

static const char *TAG = "WiFi_Prov";
static httpd_handle_t server = NULL;

/* HTML 内容 */
const char* index_html = 
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<meta charset=\"UTF-8\">"
    "<title>ESP8266 Config</title>"
    "<style>"
    "  body { font-family: sans-serif; padding: 20px; }"
    "  input { padding: 5px; margin: 5px 0; width: 100%; box-sizing: border-box; }"
    "  .advanced { margin-top: 15px; padding: 10px; border: 1px solid #ccc; background: #f9f9f9; }"
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
    "</script>"
    "</head>"
    "<body>"
    "<h2>WiFi Configuration</h2>"
    "<form action=\"/config\" method=\"post\" onsubmit=\"return validateForm()\">"
    "SSID:<br><input type=\"text\" id=\"ssid\" name=\"ssid\" maxlength=\"32\" placeholder=\"Enter SSID\"><br>"
    "Password:<br><input type=\"text\" id=\"password\" name=\"password\" maxlength=\"63\" placeholder=\"Enter Password\"><br>"
    "<div class=\"advanced\">"
    "  <strong>Advanced Settings</strong><br>"
    "  <label><input type=\"checkbox\" id=\"bssid_enable\" name=\"bssid_enable\" onclick=\"toggleAdvanced()\"> Lock BSSID</label>"
    "  <div id=\"bssid_div\" style=\"display:none\">"
    "    BSSID:<br><input type=\"text\" id=\"bssid\" name=\"bssid\" maxlength=\"17\" placeholder=\"AA:BB:CC:DD:EE:FF\">"
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
        
        // 如果 Server 存在，说明是从配网模式连接成功的，需要重启进入纯 STA 模式
        if (server) {
            ESP_LOGI(TAG, "Provisioning Successful! Restarting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
}

/* HTTP GET Handler */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

/* HTTP POST Handler */
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
static const httpd_uri_t config_uri = { .uri = "/config", .method = HTTP_POST, .handler = wifi_config_post_handler, .user_ctx = NULL };

static void start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    
    ESP_LOGI(TAG, "Starting webserver on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
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
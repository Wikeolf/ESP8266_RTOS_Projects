/* * ESP32 SoftAP + HTTP Server 配网示例
 * 基于 ESP-IDF softAP 和 http_server simple 示例合并开发
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h> // 需要这个头文件支持 URL 解码
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include <sys/param.h>

#define TAG "SoftAP_Prov"

#define AP_SSID "ESP32_Config"
#define AP_PASS "" 

const char* index_html = 
    "<!DOCTYPE html>"
    "<html>"
    "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><meta charset=\"UTF-8\"><title>ESP32 Config</title></head>"
    "<body>"
    "<h2>WiFi Configuration</h2>"
    "<form action=\"/config\" method=\"post\">"
    "SSID:<br><input type=\"text\" name=\"ssid\"><br>"
    "Password:<br><input type=\"text\" name=\"password\"><br><br>"
    "<input type=\"submit\" value=\"Connect\">"
    "</form>"
    "</body>"
    "</html>";

static httpd_handle_t server = NULL;

/* 工具函数：URL 解码 
 * 将 %C9%CF 转换为对应的原始字节
 */
void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

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
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Station started");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Failed to connect to router");
    }
}

void wifi_init_softap_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

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
}

esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

/* * 核心修改部分 * */
esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    // 1. 增加缓冲区大小，至少要比 content_len 大
    // 为了安全，我们动态分配，或者设置一个足够大的静态值 (比如 512)
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = malloc(total_len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    // 2. 完整读取数据
    int received = 0;
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            free(buf);
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    ESP_LOGI(TAG, "Received Raw Data: %s", buf);

    // 3. 准备足够大的临时缓冲区来存放 URL 编码后的字符串
    // URL 编码后的长度通常是原长度的3倍，所以我们给 100 字节肯定够用了
    char ssid_encoded[128] = {0}; 
    char password_encoded[128] = {0};

    // 4. 解析参数 (注意：这里使用更大的 buffer 接收 encoded 字符串)
    if (httpd_query_key_value(buf, "ssid", ssid_encoded, sizeof(ssid_encoded)) == ESP_OK &&
        httpd_query_key_value(buf, "password", password_encoded, sizeof(password_encoded)) == ESP_OK) {
        
        // 5. 进行 URL 解码
        char ssid_decoded[33] = {0}; // Wi-Fi SSID 最大 32 字节 + 1 结束符
        char password_decoded[65] = {0}; // Wi-Fi 密码最大 64 字节 + 1 结束符

        url_decode(ssid_decoded, ssid_encoded);
        url_decode(password_decoded, password_encoded);

        ESP_LOGI(TAG, "Decoded SSID: %s", ssid_decoded);
        ESP_LOGI(TAG, "Decoded Password: %s", password_decoded);

        wifi_config_t wifi_config = {0};
        // 确保不会溢出
        strncpy((char*)wifi_config.sta.ssid, ssid_decoded, sizeof(wifi_config.sta.ssid)); 
        strncpy((char*)wifi_config.sta.password, password_decoded, sizeof(wifi_config.sta.password));
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        
        ESP_LOGI(TAG, "Connecting to router...");
        esp_wifi_connect();

        const char *resp_str = "Connecting... Please check device status.";
        httpd_resp_send(req, resp_str, strlen(resp_str));
    } else {
        const char *resp_str = "Invalid parameters (Parsing failed)";
        httpd_resp_send(req, resp_str, strlen(resp_str));
    }

    free(buf); // 别忘了释放内存
    return ESP_OK;
}

httpd_uri_t root_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

httpd_uri_t config_uri = {
    .uri       = "/config",
    .method    = HTTP_POST,
    .handler   = wifi_config_post_handler,
    .user_ctx  = NULL
};

void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // 增加 URI 匹配长度和头部长度限制，防止某些浏览器发送超长请求导致问题
    config.max_uri_handlers = 8;
    config.stack_size = 8192; // 增加栈大小，防止处理长字符串时栈溢出

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &config_uri);
    } else {
        ESP_LOGE(TAG, "Error starting server!");
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init_softap_sta();
    start_webserver();
    
    ESP_LOGI(TAG, "System ready. Connect to AP 'ESP32_Config' and visit 192.168.4.1");
}
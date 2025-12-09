/* * ESP32 SoftAP + HTTP Server 配网示例
 * 基于 ESP-IDF softAP 和 http_server simple 示例合并开发
 */

#include <string.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#define TAG "SoftAP_Prov"

// 硬编码 AP 的配置 (手机连接这个热点进行配网)
#define AP_SSID "ESP32_Config"
#define AP_PASS "" // 开放热点，无需密码

/* * 简单的 HTML 页面，包含 SSID 和 Password 输入框 
 * 注意：实际开发中建议放在外部文件或压缩，这里为了演示直接硬编码
 */
const char* index_html = 
    "<!DOCTYPE html>"
    "<html>"
    "<head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><title>ESP32 Config</title></head>"
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

/* * WiFi 事件处理函数 
 * 处理 AP 模式下的客户端连接/断开
 * 处理 Station 模式下的连接路由器结果
 */
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
        // 这里可以添加逻辑：配网成功，关闭 AP 或通知用户
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Failed to connect to router");
        // 可以在这里添加重连逻辑
        // esp_wifi_connect(); 
    }
}

/* * 初始化 WiFi 为 AP+STA 混合模式 
 */
void wifi_init_softap_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 创建默认的 AP 和 STA 网络接口
    // esp_netif_create_default_wifi_ap();
    // esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        WIFI_MODE_NULL));

    // 配置 AP 参数
    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN, // 开放网络方便测试
            .channel = 1
        },
    };

    // 设置模式为 AP+STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi Initialized in AP+STA mode. SSID: %s", AP_SSID);
}

/* * HTTP GET Handler: 返回 HTML 配网页面
 */
esp_err_t root_get_handler(httpd_req_t *req)
{
    // 修改：使用 strlen 替代 HTTPD_RESP_USE_STRLEN
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

/* * HTTP POST Handler: 接收配网数据并连接
 * 期望数据格式: application/x-www-form-urlencoded (HTML表单默认格式)
 * 例如: ssid=MyWiFi&password=12345678
 */
esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    char buf[100]; // 缓冲区，假设数据不超过100字节
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 读取 POST Body
    if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0'; // 确保字符串结束符

    char ssid[32] = {0};
    char password[64] = {0};

    ESP_LOGI(TAG, "Received data: %s", buf);

    // 解析 URL 编码的参数 (利用 ESP-IDF 提供的 helper 函数)
    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) == ESP_OK &&
        httpd_query_key_value(buf, "password", password, sizeof(password)) == ESP_OK) {
        
        ESP_LOGI(TAG, "Parsed SSID: %s", ssid);
        ESP_LOGI(TAG, "Parsed Password: %s", password);

        // --- 核心配网逻辑 ---
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
        
        // 设置 Station 配置
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        
        // 开始连接
        ESP_LOGI(TAG, "Connecting to router...");
        esp_wifi_connect();

        const char *resp_str = "Connecting... Check device logs.";
        httpd_resp_send(req, resp_str, strlen(resp_str));
    } else {
        const char *resp_str = "Invalid parameters";
        httpd_resp_send(req, resp_str, strlen(resp_str));
    }

    return ESP_OK;
}

/* * 注册 URI Handlers 
 */
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

/* * 启动 Web Server 
 */
void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

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
    // 1. 初始化 NVS (Flash 存储)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化 WiFi (SoftAP + Station)
    wifi_init_softap_sta();

    // 3. 启动 HTTP Server
    start_webserver();
    
    ESP_LOGI(TAG, "System ready. Connect to AP 'ESP32_Config' and visit 192.168.4.1");
}
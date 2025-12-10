/* * ESP8266 SoftAP + HTTP Server 配网示例
 * 基于 ESP8266_RTOS_SDK SoftAP 和 http_server simple 示例合并开发
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h" 
#include <sys/param.h>

#define TAG "SoftAP_Prov"

#define AP_SSID "ESP8266_Config"
#define AP_PASS "" 

#define FLASH_BUTTON_GPIO GPIO_NUM_0
#define RESET_HOLD_TIME_MS 3000

void start_webserver();

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
    "    if (ssid.length == 0) {"
    "      alert('SSID cannot be empty');"
    "      return false;"
    "    }"
    "    if (ssid.length > 32) {"
    "      alert('SSID too long (max 32 chars)');"
    "      return false;"
    "    }"
    "    if (pass.length > 63) {"
    "      alert('Password too long (max 63 chars)');"
    "      return false;"
    "    }"
    "    if (bssidCheck) {"
    "      var macRegex = /^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$/;"
    "      if (!macRegex.test(bssid)) {"
    "        alert('Invalid BSSID format (e.g., AA:BB:CC:DD:EE:FF)');"
    "        return false;"
    "      }"
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
    "SSID:<br>"
    "<input type=\"text\" id=\"ssid\" name=\"ssid\" maxlength=\"32\" placeholder=\"Enter SSID\"><br>"
    "Password:<br>"
    "<input type=\"text\" id=\"password\" name=\"password\" maxlength=\"63\" placeholder=\"Enter Password\"><br>"
    
    "<div class=\"advanced\">"
    "  <strong>Advanced Settings</strong><br>"
    "  <label><input type=\"checkbox\" id=\"bssid_enable\" name=\"bssid_enable\" onclick=\"toggleAdvanced()\"> Lock BSSID (Optional)</label>"
    "  <div id=\"bssid_div\" style=\"display:none\">"
    "    BSSID (XX:XX:XX:XX:XX:XX):<br>"
    "    <input type=\"text\" id=\"bssid\" name=\"bssid\" maxlength=\"17\" placeholder=\"AA:BB:CC:DD:EE:FF\">"
    "  </div>"
    "</div>"
    
    "<br><input type=\"submit\" value=\"Connect\">"
    "</form>"
    "</body>"
    "</html>";

static httpd_handle_t server = NULL;

/* * 安全增强版 URL 解码函数
 * 增加了 dst_len 参数，防止缓冲区溢出
 */
void url_decode(char *dst, const char *src, size_t dst_len)
{
    char a, b;
    size_t written = 0;
    
    // 预留一个字节给结尾的 '\0'
    size_t max_written = dst_len - 1; 

    while (*src && written < max_written) {
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
        written++;
    }
    *dst = '\0'; // 确保必定以 null 结尾
}

/* 解析 MAC 地址字符串 (XX:XX:XX:XX:XX:XX) 到 uint8_t 数组 */
bool parse_mac_address(const char *str, uint8_t mac[6])
{
    unsigned int bytes[6];
    int count = sscanf(str, "%x:%x:%x:%x:%x:%x", 
                       &bytes[0], &bytes[1], &bytes[2], 
                       &bytes[3], &bytes[4], &bytes[5]);
    if (count == 6) {
        for (int i = 0; i < 6; i++) {
            mac[i] = (uint8_t)bytes[i];
        }
        return true;
    }
    return false;
}

/* --- 物理按键监测任务 (适配 FLASH 按键) --- */
void reset_button_task(void *arg)
{
    // 初始化 GPIO 0 (FLASH Button)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << FLASH_BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1; // NodeMCU 外部有上拉，开启内部上拉作为双重保险
    io_conf.pull_down_en = 0;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Button Task Started. Use NodeMCU 'FLASH' button (GPIO 0) to reset.");

    int hold_time = 0;
    const int check_interval = 100; // 每100ms检查一次

    while (1) {
        // FLASH 按键按下时，GPIO 0 为低电平 (0)
        if (gpio_get_level(FLASH_BUTTON_GPIO) == 0) {
            hold_time += check_interval;
            
            // 每秒打印一次提示
            if (hold_time > 0 && hold_time % 1000 == 0) {
                ESP_LOGI(TAG, "FLASH button held for %d ms...", hold_time);
            }

            // 如果按下超过 3 秒
            if (hold_time >= RESET_HOLD_TIME_MS) {
                ESP_LOGW(TAG, "Factory Reset Triggered via FLASH button!");
                
                // 1. 擦除 WiFi 配置
                nvs_flash_erase();
                nvs_flash_init(); // 重新初始化一下
                
                // 2. 提示重启
                ESP_LOGW(TAG, "WiFi credentials erased. Rebooting in 3 seconds...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                
                // 3. 重启
                esp_restart();
            }
        } else {
            // 按键松开，清零计时器
            hold_time = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(check_interval));
    }
}

//WiFi 事件处理
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    // 当有设备连接SoftAp时打印相关信息
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d", MAC2STR(event->mac), event->aid);
    } 
    // 当有设备断开SoftAp时打印相关信息
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
    }
    // 当 Station 启动时
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Station started");
    }
    // 当 Station 成功连接到路由器时
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        /* --- 配网成功后重启 --- */
        
        // 只有当 server 不为 NULL 时，说明当前是配网模式
        // 如果是正常 NVS 启动，server 是 NULL，不会进入此逻辑，从而避免死循环
        if (server) {
            ESP_LOGI(TAG, "Provisioning Successful! Restarting system to apply STA-only mode...");
            
            // 简单延时，确保串口日志打印完毕，并给浏览器一点时间接收 "Connecting..." 的响应
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            // 直接重启
            esp_restart();
        }
        /* ---------------------------- */
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Failed to connect to router");
        // 注意：如果你需要在连接失败时自动重连，可以在这里调用 esp_wifi_connect()
        // 但为了避免死循环，最好加上重试次数限制
    }
}

void wifi_init_softap_sta(void)
{
    //网口初始化
    ESP_ERROR_CHECK(esp_netif_init());
    //创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    //以默认配置初始化wifi接口
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    //注册WiFi和IP事件处理函数
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    /* --- NVS 检查逻辑 --- */
    wifi_config_t wifi_config;
    // 从 NVS 加载当前配置到 RAM
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));

    if (wifi_config.sta.ssid[0] != 0) {
        ESP_LOGI(TAG, "NVS config found (SSID: %s). Starting in STA Mode.", wifi_config.sta.ssid);
        if (wifi_config.sta.bssid_set) {
            ESP_LOGI(TAG, "Locking to BSSID: "MACSTR, MAC2STR(wifi_config.sta.bssid));
        }
        // 1. 设置为仅 Station 模式
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        // 2. 启动 WiFi
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    else {
         ESP_LOGI(TAG, "No NVS config found. Starting in AP+STA Mode for Provisioning.");
         //配置SoftAP参数
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
         //设置WiFi工作模式为AP+STA
         ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
         //设置SoftAP配置
         ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
         //启动WiFi
         ESP_ERROR_CHECK(esp_wifi_start());

         //启动WebServer
         start_webserver();
        ESP_LOGI(TAG, "SoftAP '%s' started. Connect to configure WiFi.", AP_SSID);
    }

}

// 辅助函数：处理根目录的 GET 请求，返回 HTML 页面
esp_err_t root_get_handler(httpd_req_t *req)
{
    // 发送 HTML 页面
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

// 辅助函数：处理 /config 的 POST 请求，解析 SSID 和密码并连接到路由器
esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    //获取请求内容长度
    int total_len = req->content_len;
    int cur_len = 0;
    //分配缓冲区存储请求体
    char *buf = malloc(total_len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    int received = 0;
    //循环接收数据直到完整
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

    char ssid_encoded[128] = {0}; 
    char password_encoded[128] = {0};
    char bssid_encoded[64] = {0};
    char bssid_enable[10] = {0};

    // 只要有 SSID 和 Password 就认为是有效请求，BSSID 是可选的
    if (httpd_query_key_value(buf, "ssid", ssid_encoded, sizeof(ssid_encoded)) == ESP_OK &&
        httpd_query_key_value(buf, "password", password_encoded, sizeof(password_encoded)) == ESP_OK) {
        
        char ssid_decoded[33] = {0};
        char password_decoded[65] = {0};

        url_decode(ssid_decoded, ssid_encoded, sizeof(ssid_decoded));
        url_decode(password_decoded, password_encoded, sizeof(password_decoded));

        ESP_LOGI(TAG, "Decoded SSID: %s", ssid_decoded);
        ESP_LOGI(TAG, "Decoded Password: %s", password_decoded);

        //配置并连接到路由器
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, ssid_decoded, sizeof(wifi_config.sta.ssid)); 
        strncpy((char*)wifi_config.sta.password, password_decoded, sizeof(wifi_config.sta.password));
        
        /* --- 处理 BSSID 锁定逻辑 --- */
        // 检查 checkbox 是否勾选 (HTML checkbox 选中时会发送 "bssid_enable=on"，否则不发送)
        if (httpd_query_key_value(buf, "bssid_enable", bssid_enable, sizeof(bssid_enable)) == ESP_OK &&
            strcmp(bssid_enable, "on") == 0) {
            
            if (httpd_query_key_value(buf, "bssid", bssid_encoded, sizeof(bssid_encoded)) == ESP_OK) {
                char bssid_decoded[20] = {0};
                url_decode(bssid_decoded, bssid_encoded, sizeof(bssid_decoded));
                
                // 解析 MAC 地址
                if (parse_mac_address(bssid_decoded, wifi_config.sta.bssid)) {
                    wifi_config.sta.bssid_set = true;
                    ESP_LOGI(TAG, "BSSID Locking Enabled: "MACSTR, MAC2STR(wifi_config.sta.bssid));
                } else {
                    ESP_LOGW(TAG, "Invalid BSSID format received, ignoring lock.");
                }
            }
        }
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        
        ESP_LOGI(TAG, "Connecting to router...");
        esp_wifi_connect();
        
        // 回复客户端
        const char *resp_str = "Connecting... Please check device status.";
        httpd_resp_send(req, resp_str, strlen(resp_str));
    } else {
        // 解析失败，回复错误信息
        const char *resp_str = "Invalid parameters (Parsing failed)";
        httpd_resp_send(req, resp_str, strlen(resp_str));
    }

    free(buf); 
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

void start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192; 

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

    // 1. 启动按键监测任务 (任务栈大小 2048字节，优先级 10)
    xTaskCreate(reset_button_task, "reset_btn_task", 2048, NULL, 10, NULL);

    wifi_init_softap_sta();
    
    ESP_LOGI(TAG, "System ready.");
}
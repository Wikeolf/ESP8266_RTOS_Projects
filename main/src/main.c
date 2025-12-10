/* ESP8266 SoftAP + HTTP Server 配网示例 */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 引入自定义模块
#include "wifi_prov.h"
#include "peripherals.h"

static const char *TAG = "Main";

void app_main(void)
{
    // 1. 初始化 NVS (WiFi 配置和系统参数存储在这里)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "System Init...");

    // 2. 启动按键监测任务 (来自 peripherals 模块)
    // 任务栈大小 2048字节，优先级 10
    xTaskCreate(reset_button_task, "reset_btn_task", 2048, NULL, 10, NULL);

    // 3. 启动 WiFi 逻辑 (根据 NVS 自动决定是 STA 还是 配网模式)
    wifi_init_softap_sta();
    
    ESP_LOGI(TAG, "System ready.");
}
#include "peripherals.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

static const char *TAG = "Peripherals";

void reset_button_task(void *arg)
{
    // 初始化 GPIO 0 (FLASH Button)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << FLASH_BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    // NodeMCU 外部有上拉，开启内部上拉作为双重保险
    io_conf.pull_up_en = 1; 
    io_conf.pull_down_en = 0;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Button Monitor Task Started. GPIO %d", FLASH_BUTTON_GPIO);

    int hold_time = 0;
    const int check_interval = 100; // 100ms 轮询一次

    while (1) {
        // FLASH 按键按下时，GPIO 0 为低电平 (0)
        if (gpio_get_level(FLASH_BUTTON_GPIO) == 0) {
            hold_time += check_interval;
            
            // 每秒打印一次提示，方便调试
            if (hold_time > 0 && hold_time % 1000 == 0) {
                ESP_LOGI(TAG, "Button held for %d ms...", hold_time);
            }

            // 如果按下超过阈值
            if (hold_time >= RESET_HOLD_TIME_MS) {
                ESP_LOGW(TAG, "Factory Reset Triggered via Button!");
                
                // 1. 擦除 WiFi 配置
                nvs_flash_erase();
                nvs_flash_init(); // 重新初始化 NVS 保证状态正确
                
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
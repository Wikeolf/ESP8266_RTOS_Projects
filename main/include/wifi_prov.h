#ifndef WIFI_PROV_H
#define WIFI_PROV_H

// SoftAP 配置
#define AP_SSID "ESP8266_Config"
#define AP_PASS "" 

/**
 * @brief 初始化 WiFi 逻辑
 * * 自动检测 NVS 中是否存在 WiFi 配置：
 * - 存在配置 -> 进入 STA 模式连接路由
 * - 不存在配置 -> 进入 AP+STA 模式开启配网 WebServer
 */
void wifi_init_softap_sta(void);

#endif // WIFI_PROV_H
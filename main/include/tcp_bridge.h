#ifndef TCP_BRIDGE_H
#define TCP_BRIDGE_H

/**
 * @brief 初始化 TCP 到 UART 的透传桥接
 * * 硬件连接:
 * - ESP8266 D7 (GPIO13) <--> 目标 TX
 * - ESP8266 D8 (GPIO15) <--> 目标 RX
 * * 逻辑:
 * - 启动 TCP Server (Port 8888)
 * - 客户端连接后，启动双任务全双工透传
 */
void tcp_bridge_init(void);

#endif // TCP_BRIDGE_H
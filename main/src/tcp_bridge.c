#include "tcp_bridge.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"

static const char *TAG = "TCP_Bridge";

// === 配置参数 ===
#define TCP_PORT 8888
#define UART_NUM UART_NUM_0
#define BRIDGE_BAUDRATE 115200
#define BUF_SIZE 512       // 缓冲区大小

// === 共享上下文结构体 ===
typedef struct {
    int sock;               // 当前连接的 Socket
    SemaphoreHandle_t exit_sem; // 用于通知连接结束的信号量
    volatile bool running;  // 运行标志位
} bridge_context_t;

// ====================================================
// 任务 1: Socket -> UART (下行数据)
// 阻塞在 recv 上，收到网络数据立刻写入串口
// ====================================================
static void tcp_to_uart_task(void *pvParameters) {
    bridge_context_t *ctx = (bridge_context_t *)pvParameters;
    uint8_t *buffer = (uint8_t *)malloc(BUF_SIZE);
    
    if (!buffer) {
        ESP_LOGE(TAG, "Tx Task malloc failed");
        xSemaphoreGive(ctx->exit_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Task [Net->UART] started");

    while (ctx->running) {
        // 阻塞接收网络数据
        int len = recv(ctx->sock, buffer, BUF_SIZE, 0);

        if (len > 0) {
            // 写入串口
            uart_write_bytes(UART_NUM, (const char *)buffer, len);
        } else {
            // len == 0 (连接关闭) 或 len < 0 (错误)
            if (ctx->running) { // 只有在理应运行时才报错
                ESP_LOGW(TAG, "Socket read failed or disconnected (err: %d)", errno);
            }
            break; // 跳出循环，触发清理
        }
    }

    free(buffer);
    ctx->running = false; // 标记停止，通知另一个任务
    xSemaphoreGive(ctx->exit_sem); // 释放信号量通知主任务
    vTaskDelete(NULL);
}

// ====================================================
// 任务 2: UART -> Socket (上行数据)
// 循环读取串口，有数据就发给 Socket
// ====================================================
static void uart_to_tcp_task(void *pvParameters) {
    bridge_context_t *ctx = (bridge_context_t *)pvParameters;
    uint8_t *buffer = (uint8_t *)malloc(BUF_SIZE);
    
    if (!buffer) {
        ESP_LOGE(TAG, "Rx Task malloc failed");
        xSemaphoreGive(ctx->exit_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Task [UART->Net] started");

    while (ctx->running) {
        // 读取串口数据
        // 设置 100ms 超时，这样即使没有串口数据，也能每 100ms 检查一次 ctx->running 状态
        // 避免 Socket 断开后，这个任务死锁在 uart_read_bytes 上
        size_t buffered_len = 0;
        uart_get_buffered_data_len(UART_NUM, &buffered_len);

        if (buffered_len > 0) {
            int read_len = uart_read_bytes(UART_NUM, buffer, 
                                           (buffered_len > BUF_SIZE ? BUF_SIZE : buffered_len), 
                                           100 / portTICK_RATE_MS);
            if (read_len > 0) {
                int sent = send(ctx->sock, buffer, read_len, 0);
                if (sent < 0) {
                    ESP_LOGE(TAG, "Socket send failed (errno: %d)", errno);
                    break;
                }
            }
        } else {
            // 没有数据，短暂延时检查运行标志
            vTaskDelay(100 / portTICK_RATE_MS);
        }
    }

    free(buffer);
    ctx->running = false;
    xSemaphoreGive(ctx->exit_sem);
    vTaskDelete(NULL);
}

// ====================================================
// 主 Server 任务: 接受连接并调度子任务
// ====================================================
static void tcp_server_task(void *pvParameters) {
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(TCP_PORT);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        vTaskDelete(NULL);
        return;
    }

    bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    listen(listen_sock, 1);

    ESP_LOGI(TAG, "Bridge Server listening on port %d...", TCP_PORT);

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        
        // 1. 等待客户端连接
        ESP_LOGI(TAG, "Waiting for client...");
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Accept failed");
            vTaskDelay(1000 / portTICK_RATE_MS);
            continue;
        }
        
        ESP_LOGI(TAG, "Client connected! Spawning tasks.");

        // 2. 初始化会话上下文
        bridge_context_t ctx;
        ctx.sock = sock;
        ctx.running = true;
        ctx.exit_sem = xSemaphoreCreateCounting(2, 0); // 最多允许两个任务释放

        // 3. 创建双向透传任务
        // 栈大小设为 2048 字节，根据实际情况调整
        xTaskCreate(tcp_to_uart_task, "tcp2uart", 2048, &ctx, 5, NULL);
        xTaskCreate(uart_to_tcp_task, "uart2tcp", 2048, &ctx, 5, NULL);

        // 4. 等待结束信号
        // 只要有任意一个任务退出（释放信号量），说明连接断开或出错
        xSemaphoreTake(ctx.exit_sem, portMAX_DELAY);

        ESP_LOGW(TAG, "Session ending. Cleaning up...");

        // 5. 强制关闭逻辑
        ctx.running = false; // 通知剩下的那个任务停止
        
        // 关闭 Socket 会导致阻塞在 recv() 的 tcp_to_uart_task 立即返回错误并退出
        shutdown(sock, 0);
        close(sock);

        // 稍作延时，确保两个子任务都有时间执行清理并自杀
        vTaskDelay(500 / portTICK_RATE_MS);
        
        vSemaphoreDelete(ctx.exit_sem);
        ESP_LOGI(TAG, "Cleaned up. Ready for next.");
    }

    vTaskDelete(NULL);
}

void tcp_bridge_init(void) {
    // 1. 配置 UART
    uart_config_t uart_config = {
        .baud_rate = BRIDGE_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    
    // 安装驱动
    // TX buffer 设为 0 因为我们是用阻塞写入，RX buffer 设为 1024
    uart_driver_install(UART_NUM, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);

    // 2. 关键：交换引脚到 D7(RX) / D8(TX)
    uart_enable_swap();
    ESP_LOGI(TAG, "UART Swapped: TX->D8(GPIO15), RX->D7(GPIO13)");

    // 3. 启动 Server 主任务
    xTaskCreate(tcp_server_task, "bridge_server", 3072, NULL, 5, NULL);
}
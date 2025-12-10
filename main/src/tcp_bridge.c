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
#define BUF_SIZE 512       // 单次读写临时缓冲区大小

// 离线缓存大小 (8KB)
// 请根据 ESP8266 剩余内存实际情况调整，太大会导致 malloc 失败
#define UART_CACHE_SIZE (8 * 1024) 

// === 共享上下文结构体 ===
typedef struct {
    int sock;               
    SemaphoreHandle_t exit_sem; 
    volatile bool running;  
} bridge_context_t;

// === 环形缓冲区结构 ===
typedef struct {
    uint8_t *buffer;
    size_t size;
    size_t head; // 写指针
    size_t tail; // 读指针
    size_t count;// 当前数据量
    SemaphoreHandle_t mutex;
} ringbuf_t;

static ringbuf_t s_rb;

// 初始化环形缓冲区
static bool rb_init(size_t size) {
    s_rb.buffer = malloc(size);
    if (!s_rb.buffer) return false;
    s_rb.size = size;
    s_rb.head = 0;
    s_rb.tail = 0;
    s_rb.count = 0;
    s_rb.mutex = xSemaphoreCreateMutex();
    return true;
}

// 写入数据 (如果满了，覆盖最旧的数据)
static void rb_write(const uint8_t *data, size_t len) {
    if (!s_rb.buffer) return;
    xSemaphoreTake(s_rb.mutex, portMAX_DELAY);
    for (size_t i = 0; i < len; i++) {
        s_rb.buffer[s_rb.head] = data[i];
        s_rb.head = (s_rb.head + 1) % s_rb.size;
        
        if (s_rb.count < s_rb.size) {
            s_rb.count++;
        } else {
            // 缓冲区已满，覆盖旧数据，读指针被迫前移
            s_rb.tail = (s_rb.tail + 1) % s_rb.size;
        }
    }
    xSemaphoreGive(s_rb.mutex);
}

// 读取数据
static int rb_read(uint8_t *dst, int max_len) {
    if (!s_rb.buffer) return 0;
    xSemaphoreTake(s_rb.mutex, portMAX_DELAY);
    int read_cnt = 0;
    while (read_cnt < max_len && s_rb.count > 0) {
        dst[read_cnt++] = s_rb.buffer[s_rb.tail];
        s_rb.tail = (s_rb.tail + 1) % s_rb.size;
        s_rb.count--;
    }
    xSemaphoreGive(s_rb.mutex);
    return read_cnt;
}

// ====================================================
// 守护任务: 持续从串口读取数据到环形缓冲区
// 即使没有 TCP 连接，这个任务也在后台运行
// ====================================================
static void uart_rx_daemon_task(void *arg) {
    uint8_t *tmp_buf = (uint8_t *)malloc(BUF_SIZE);
    if (!tmp_buf) {
        ESP_LOGE(TAG, "Daemon malloc failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UART Capture Daemon Started (Cache: %d bytes)", UART_CACHE_SIZE);

    while (1) {
        // 读取串口 (设置较短的超时，保证任务不完全阻塞)
        // 注意：ESP8266 RTOS SDK 中 portTICK_RATE_MS 可能为 1 或 10
        int len = uart_read_bytes(UART_NUM, tmp_buf, BUF_SIZE, 20 / portTICK_RATE_MS);
        if (len > 0) {
            rb_write(tmp_buf, len);
        } else {
            // 没有数据时稍微让出 CPU
            vTaskDelay(10 / portTICK_RATE_MS);
        }
    }
    free(tmp_buf); // Unreachable
}

// ====================================================
// 任务 1: Socket -> UART (下行数据 - 保持原样)
// ====================================================
static void tcp_to_uart_task(void *pvParameters) {
    bridge_context_t *ctx = (bridge_context_t *)pvParameters;
    uint8_t *buffer = (uint8_t *)malloc(BUF_SIZE);
    
    if (!buffer) {
        xSemaphoreGive(ctx->exit_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Task [Net->UART] started");

    while (ctx->running) {
        int len = recv(ctx->sock, buffer, BUF_SIZE, 0);
        if (len > 0) {
            uart_write_bytes(UART_NUM, (const char *)buffer, len);
        } else {
            if (ctx->running) {
                ESP_LOGW(TAG, "Socket read failed or disconnected");
            }
            break; 
        }
    }

    free(buffer);
    ctx->running = false;
    xSemaphoreGive(ctx->exit_sem);
    vTaskDelete(NULL);
}

// ====================================================
// 任务 2: Buffer -> Socket (上行数据 - 已修改)
// 现在改为从 RingBuffer 读取，而不是直接读串口
// ====================================================
static void buffer_to_tcp_task(void *pvParameters) {
    bridge_context_t *ctx = (bridge_context_t *)pvParameters;
    uint8_t *buffer = (uint8_t *)malloc(BUF_SIZE);
    
    if (!buffer) {
        xSemaphoreGive(ctx->exit_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Task [Cache->Net] started. Flushing buffer...");

    while (ctx->running) {
        // 从环形缓冲区取数据
        int len = rb_read(buffer, BUF_SIZE);

        if (len > 0) {
            // 发送数据
            int sent = send(ctx->sock, buffer, len, 0);
            if (sent < 0) {
                ESP_LOGE(TAG, "Socket send failed (errno: %d)", errno);
                break;
            }
        } else {
            // 缓冲区空，等待新数据
            // 50ms 轮询一次，兼顾响应速度和 CPU 占用
            vTaskDelay(50 / portTICK_RATE_MS);
        }
    }

    free(buffer);
    ctx->running = false;
    xSemaphoreGive(ctx->exit_sem);
    vTaskDelete(NULL);
}

// ====================================================
// 主 Server 任务
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
        
        ESP_LOGI(TAG, "Waiting for client...");
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Accept failed");
            vTaskDelay(1000 / portTICK_RATE_MS);
            continue;
        }
        
        ESP_LOGI(TAG, "Client connected! Starting tasks.");

        bridge_context_t ctx;
        ctx.sock = sock;
        ctx.running = true;
        ctx.exit_sem = xSemaphoreCreateCounting(2, 0);

        // 创建透传任务
        // 注意：原 uart_to_tcp 改为 buffer_to_tcp
        xTaskCreate(tcp_to_uart_task, "tcp2uart", 2048, &ctx, 5, NULL);
        xTaskCreate(buffer_to_tcp_task, "buf2tcp", 2048, &ctx, 5, NULL);

        xSemaphoreTake(ctx.exit_sem, portMAX_DELAY);

        ESP_LOGW(TAG, "Session ending. Cleaning up...");

        ctx.running = false; 
        shutdown(sock, 0);
        close(sock);

        vTaskDelay(500 / portTICK_RATE_MS);
        vSemaphoreDelete(ctx.exit_sem);
        ESP_LOGI(TAG, "Cleaned up. Ready for next.");
    }

    vTaskDelete(NULL);
}

void tcp_bridge_init(void) {
    // 1. 初始化环形缓冲区
    if (!rb_init(UART_CACHE_SIZE)) {
        ESP_LOGE(TAG, "Failed to allocate UART cache buffer!");
        return;
    }

    // 2. 配置 UART
    uart_config_t uart_config = {
        .baud_rate = BRIDGE_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    
    // 安装驱动 (Rx buffer 1024, Tx buffer 0)
    uart_driver_install(UART_NUM, 1024, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);

    // 3. 交换引脚
    uart_enable_swap();
    ESP_LOGI(TAG, "UART Swapped: TX->D8(GPIO15), RX->D7(GPIO13)");

    // 4. 启动永久运行的串口接收守护任务
    // 优先级略高于普通任务，防止数据丢失
    xTaskCreate(uart_rx_daemon_task, "uart_daemon", 2048, NULL, 10, NULL);

    // 5. 启动 TCP Server
    xTaskCreate(tcp_server_task, "bridge_server", 3072, NULL, 5, NULL);
}
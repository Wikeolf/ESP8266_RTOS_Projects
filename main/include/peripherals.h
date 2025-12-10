#ifndef PERIPHERALS_H
#define PERIPHERALS_H

#include <stdint.h>

// 定义按键 GPIO (NodeMCU FLASH Button)
#define FLASH_BUTTON_GPIO 0
// 长按重置时间 (毫秒)
#define RESET_HOLD_TIME_MS 3000

/**
 * @brief 物理按键监测任务
 * 监测 GPIO 0 (Flash Button)，长按指定时间后触发 NVS 擦除并重启
 * @param arg 任务参数 (未使用)
 */
void reset_button_task(void *arg);

#endif // PERIPHERALS_H
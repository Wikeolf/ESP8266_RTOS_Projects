#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief 安全 URL 解码函数
 * @param dst 目标缓冲区
 * @param src 源字符串
 * @param dst_len 目标缓冲区大小
 */
void url_decode(char *dst, const char *src, size_t dst_len);

/**
 * @brief 解析 MAC 地址字符串 (XX:XX:XX:XX:XX:XX)
 * @param str 输入字符串
 * @param mac 输出 uint8_t[6] 数组
 * @return true 解析成功, false 失败
 */
bool parse_mac_address(const char *str, uint8_t mac[6]);

#endif // UTILS_H
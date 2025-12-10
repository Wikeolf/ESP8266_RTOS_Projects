#include "utils.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// 注意：这里不再需要包含 FreeRTOS 或 driver/gpio.h，因为它现在只处理纯逻辑

void url_decode(char *dst, const char *src, size_t dst_len)
{
    char a, b;
    size_t written = 0;
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
    *dst = '\0';
}

bool parse_mac_address(const char *str, uint8_t mac[6])
{
    unsigned int bytes[6];
    // 使用 sscanf 解析标准 MAC 格式
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
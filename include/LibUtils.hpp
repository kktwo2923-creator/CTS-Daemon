#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <cstring>
#include <cstdint>

namespace LibUtils {
    // 支持可选前导负号；非数字字符即停（与原行为一致）
    static int Fastatoi(const char* str) {
        bool neg = false;
        if (*str == '-') {
            neg = true;
            ++str;
        }
        unsigned int result = 0;
        while (*str >= '0' && *str <= '9') {
            result = result * 10 + (*str - '0');
            ++str;
        }
        return neg ? -(int)result : (int)result;
    }

    static long Faststrlen(const char* str) {
        long len = 0;
        while (str[len] != '\0') {
            ++len;
        }
        return len;
    }

    static char* emit_u32(char *buf, char *end, uint32_t val) {
        char tmp[11];
        char *out = tmp + sizeof(tmp);
    
        do {
            *--out = (char)('0' + (val % 10u));
            val /= 10u;
        } while (val);
        
        const size_t len = (size_t)(tmp + sizeof(tmp) - out);
    
        const size_t avail = (end > buf) ? (size_t)(end - buf) : 0;
        const size_t copy = len < avail ? len : avail;
    
        memcpy(buf, out, copy);
    
        return buf + copy;
    }

    static int FastVsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
        char *p = buf;
        char *const end = buf + (size ? size : (size_t)-1);
    
        while (*fmt) {
            if (*fmt != '%') {
                if (p < end) *p = *fmt;
                ++p; ++fmt;
                continue;
            }
            ++fmt;                          
            switch (*fmt++) {
            case 'd': {
                // 原实现对负数输出天文数字；取负用 unsigned 运算避免 INT_MIN 溢出 UB
                int v = va_arg(ap, int);
                if (v < 0) {
                    if (p < end) *p = '-';
                    ++p;
                    p = emit_u32(p, end, (uint32_t)(0u - (unsigned int)v));
                } else {
                    p = emit_u32(p, end, (uint32_t)v);
                }
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                size_t len = strlen(s);
                size_t avail = (end > p) ? (size_t)(end - p) : 0;
                size_t cp = (len < avail) ? len : avail;
                if (cp) memcpy(p, s, cp);
                p += cp;
                break;
            }
            case '%':
                if (p < end) *p = '%';
                ++p;
                break;
            default :
                fprintf(stderr, "未知类型");
                break;
            }
        }
    
        if (size) {                       
            if (p >= end) p = end - 1;
            *p = '\0';
        }
        return (int)(p - buf);              
    }
    
    inline int FastSnprintf(char* buf, size_t size, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int r = FastVsnprintf(buf, size, fmt, ap);
        va_end(ap);
        return r;
    }
};
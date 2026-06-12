#pragma once 

#include "Utils.hpp"
#include "LibUtils.hpp"

class Logger {
private:
    static constexpr const char* logpath = "/sdcard/Android/CTS/log.txt";

    constexpr static int LINE_SIZE = 1024 * 8;   // 8 KiB，单行日志远小于此，省 3 实例 ×24KB
    char lineCache[LINE_SIZE];

    LOG_LEVEL logLevel_ = LOG_LEVEL::INFO;
    mutex logPrintMutex;
public:
    void Debug(const char* message) {
        Log(LOG_LEVEL::DEBUG, message);
    } 

    void Info(const char* message) {
        Log(LOG_LEVEL::INFO, message);
    }
    void Warn(const char* message) {
        Log(LOG_LEVEL::WARN, message);
    }

    void Error(const char* message) {
        Log(LOG_LEVEL::ERROR, message);
    }

    template<typename... Args>
    void Debug(const char* message, Args&&... args) {
        Log(LOG_LEVEL::DEBUG, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Info(const char* message, Args&&... args) {
        Log(LOG_LEVEL::INFO, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Warn(const char* message, Args&&... args) {
        Log(LOG_LEVEL::WARN, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Error(const char* message, Args&&... args) {
        Log(LOG_LEVEL::ERROR, message, std::forward<Args>(args)...);
    }

    // qlib::string operator== 已修为按内容比较；此处保守保留 strcmp（语义本就正确）
    void setLogLevel(string_t& level) {
        const char* s = level.c_str();
        if      (!strcmp(s, "DEBUG")) logLevel_ = LOG_LEVEL::DEBUG;
        else if (!strcmp(s, "INFO"))  logLevel_ = LOG_LEVEL::INFO;
        else if (!strcmp(s, "WARN"))  logLevel_ = LOG_LEVEL::WARN;
        else if (!strcmp(s, "ERROR")) logLevel_ = LOG_LEVEL::ERROR;
    }

    void clear_log() {
        // Create parent directory if needed
        mkdir("/sdcard/Android/CTS", 0755);
        mkdir("/sdcard/Android", 0755);
        auto temp = fopen(logpath, "wb");
        if (!temp) {
            fprintf(stderr, "ERROR:清理日志文件失败");
            return;
        }
        fclose(temp);
    }

private:
    // 超限时保留后半段（行对齐），避免日志突然全没
    static constexpr long LOG_MAX_SIZE = 1024 * 512;

    void WriteFile(const char* content, const int len) noexcept {
        int fd = open(logpath, O_RDWR | O_CREAT | O_APPEND, 0666);

        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size >= LOG_MAX_SIZE) {
                const long keep = LOG_MAX_SIZE / 2;
                char* buf = static_cast<char*>(malloc(keep));
                if (buf) {
                    ssize_t n = pread(fd, buf, keep, st.st_size - keep);
                    if (n > 0) {
                        long off = 0;
                        while (off < n && buf[off] != '\n') off++;
                        if (off < n) off++;
                        if (ftruncate(fd, 0) == 0)
                            write(fd, buf + off, n - off);
                    } else {
                        ftruncate(fd, 0);
                    }
                    free(buf);
                } else {
                    ftruncate(fd, 0);
                }
            }
            write(fd, content, len);
            close(fd);
        }
    }


    int getCurrentTimeStr(char* buf, size_t size) {
        time_t now = time(nullptr);
        struct tm* local_time = localtime(&now);
        return strftime(buf, size, "%Y-%m-%d %H:%M:%S ", local_time);
    }

    void Log(LOG_LEVEL level, const char* message) {
        if (level < logLevel_) return;  // 热路径 Debug 被过滤时不抢锁不格式化
        lock_guard<mutex> lock(logPrintMutex);

        {
            int len = getCurrentTimeStr(lineCache, sizeof(lineCache));
            len += FastSnprintf(lineCache + len, sizeof(lineCache) - len, "%s %s\n", levelStrings.at(level), message);

            #if DEBUG_DURATION
                printf("%s\n", lineCache);
            #endif
            WriteFile(lineCache, len);
        }
    }

    template<typename... Args>
    void Log(LOG_LEVEL level, const char* message, Args&&... args) {
        if (level < logLevel_) return;
        lock_guard<mutex> lock(logPrintMutex);

        {
            const int prefixLen = getCurrentTimeStr(lineCache, sizeof(lineCache));

            int len = prefixLen + FastSnprintf(lineCache + prefixLen, sizeof(lineCache) - prefixLen, "%s ", levelStrings.at(level));

            len += snprintf(lineCache + len, sizeof(lineCache) - len, message, std::forward<Args>(args)...);

            if (len <= prefixLen || (size_t)len >= sizeof(lineCache) - 1) {
                lineCache[prefixLen] = '\0';
                fprintf(stderr, "日志异常: len[%d] lineCache[%s]\n", len, lineCache);
                return;
            }

            lineCache[len++] = '\n';

            #if DEBUG_DURATION
                printf("%s\n", lineCache);
            #endif
            WriteFile(lineCache, len);
        }
    }


    inline static const unordered_map<LOG_LEVEL, const char*> levelStrings = {
        {LOG_LEVEL::DEBUG, "调试 ->"},
        {LOG_LEVEL::INFO, "信息 ->"},
        {LOG_LEVEL::WARN, "警告 ->"},
        {LOG_LEVEL::ERROR, "错误 ->"},
    };
};
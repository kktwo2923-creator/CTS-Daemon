#pragma once 

#include "Utils.hpp"
#include "LibUtils.hpp"

class Logger {
private:
    static constexpr const char* logpath = "/sdcard/Android/CTS/log.txt";

    constexpr static int LINE_SIZE = 1024 * 32;   //  32 KiB
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

    // [Fix] qlib::string 的 operator==(self,self) 在两个独立堆分配的字符串之间比较时
    //            存在 UB（distance 跨堆分配），导致 level == "DEBUG" 这种字面量比较
    //            实际上是先把 "DEBUG" 构造成新的 value，再用坏 friend operator== 比较 → 永远不匹配。
    //            改用 strcmp 走 c_str() 路径。
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
    static constexpr long LOG_MAX_SIZE = 1024 * 10; // 10KB auto-clear threshold

    void WriteFile(const char* content, const int len) noexcept {
        int fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0666);

        if (fd >= 0) {
            // Auto-clear log when exceeds 10KB
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size >= LOG_MAX_SIZE) {
                if (ftruncate(fd, 0) == 0) {
                    // File truncated, write will start at offset 0
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
        lock_guard<mutex> lock(logPrintMutex);

        if (level >= logLevel_) {
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
        lock_guard<mutex> lock(logPrintMutex);

        if (level >= logLevel_) {    
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
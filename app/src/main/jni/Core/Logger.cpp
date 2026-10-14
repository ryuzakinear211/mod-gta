#include "Logger.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace ModLogger {

    static const char *const CANDIDATE_PATHS[] = {
        "/storage/emulated/0/Documents/mod_gta_debug.log",
        "/storage/emulated/0/Document/mod_gta_debug.log",
        "/sdcard/Documents/mod_gta_debug.log",
        "/sdcard/mod_gta_debug.log",
        "/data/data/com.android.support/files/mod_gta_debug.log"
    };
    static const size_t NUM_CANDIDATE_PATHS = sizeof(CANDIDATE_PATHS) / sizeof(CANDIDATE_PATHS[0]);

    static char s_activeLogPath[256] = "";
    static int s_activeFd = -1;
    static std::mutex s_logMutex;
    static std::atomic<const char*> s_lastAction{"Initialized"};

    static void ensureParentDir(const char *path) {
        char dir[256];
        strncpy(dir, path, sizeof(dir) - 1);
        dir[sizeof(dir) - 1] = '\0';
        char *slash = strrchr(dir, '/');
        if (slash != nullptr) {
            *slash = '\0';
            mkdir(dir, 0777);
        }
    }

    void InitLogger() {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_activeFd >= 0) return;

        for (size_t i = 0; i < NUM_CANDIDATE_PATHS; i++) {
            const char *path = CANDIDATE_PATHS[i];
            ensureParentDir(path);
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (fd >= 0) {
                s_activeFd = fd;
                strncpy(s_activeLogPath, path, sizeof(s_activeLogPath) - 1);
                s_activeLogPath[sizeof(s_activeLogPath) - 1] = '\0';
                break;
            }
        }
    }

    const char* GetActiveLogPath() {
        return s_activeLogPath[0] ? s_activeLogPath : "Logcat Only";
    }

    void SetLastAction(const char *action) {
        if (action != nullptr) {
            s_lastAction.store(action, std::memory_order_relaxed);
        }
    }

    const char* GetLastAction() {
        return s_lastAction.load(std::memory_order_relaxed);
    }

    void LogRaw(const char *buffer, size_t len) {
        if (buffer == nullptr || len == 0) return;

        // 1. Log to Android Logcat
        __android_log_write(ANDROID_LOG_INFO, MOD_TAG, buffer);

        // 2. Write to single active log file (if available)
        if (s_activeFd >= 0) {
            write(s_activeFd, buffer, len);
        }
    }

    void Log(android_LogPriority priority, const char *fmt, ...) {
        char timeStr[32];
        time_t rawtime;
        time(&rawtime);
        struct tm *timeinfo = localtime(&rawtime);
        if (timeinfo) {
            strftime(timeStr, sizeof(timeStr), "%H:%M:%S", timeinfo);
        } else {
            snprintf(timeStr, sizeof(timeStr), "N/A");
        }

        char msgBuf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
        va_end(args);

        // Fast Android Logcat print
        __android_log_print(priority, MOD_TAG, "[%s] %s", timeStr, msgBuf);

        // Thread-safe buffered append to active file
        if (s_activeFd >= 0) {
            std::lock_guard<std::mutex> lock(s_logMutex);
            char lineBuf[1100];
            int lineLen = snprintf(lineBuf, sizeof(lineBuf), "[%s] %s\n", timeStr, msgBuf);
            if (lineLen > 0) {
                write(s_activeFd, lineBuf, static_cast<size_t>(lineLen));
            }
        }
    }

} // namespace ModLogger

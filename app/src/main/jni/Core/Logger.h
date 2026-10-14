#ifndef MOD_LOGGER_H
#define MOD_LOGGER_H

#include <android/log.h>
#include <cstdarg>

#define MOD_TAG "Mod_GTA"

namespace ModLogger {

    void InitLogger();
    void Log(android_LogPriority priority, const char *fmt, ...);
    void LogRaw(const char *buffer, size_t len);
    void SetLastAction(const char *action);
    const char* GetLastAction();
    const char* GetActiveLogPath();

} // namespace ModLogger

// Convenience logging macros
#define LOG_INFO(...)  ModLogger::Log(ANDROID_LOG_INFO,  __VA_ARGS__)
#define LOG_WARN(...)  ModLogger::Log(ANDROID_LOG_WARN,  __VA_ARGS__)
#define LOG_ERROR(...) ModLogger::Log(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOG_DEBUG(...) ModLogger::Log(ANDROID_LOG_DEBUG, __VA_ARGS__)

#endif // MOD_LOGGER_H

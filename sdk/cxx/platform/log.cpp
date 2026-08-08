#include "platform/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace asternet {
namespace platform {

namespace {
std::mutex g_log_mutex;
asternet_log_callback_t g_callback = nullptr;
void *g_callback_user_data = nullptr;
int g_level = static_cast<int>(LogLevel::kInfo);
}  // namespace

void set_log_callback(asternet_log_callback_t callback, void *user_data, int level) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_callback = callback;
    g_callback_user_data = user_data;
    g_level = level;
}

void log(LogLevel level, const char *tag, const char *fmt, ...) {
    asternet_log_callback_t callback = nullptr;
    void *callback_user_data = nullptr;
    int threshold = 0;
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        callback = g_callback;
        callback_user_data = g_callback_user_data;
        threshold = g_level;
    }
    if (static_cast<int>(level) > threshold || threshold == 0) return;

    char message[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (callback != nullptr) {
        callback(static_cast<int>(level), tag, message, callback_user_data);
        return;
    }
#ifdef __ANDROID__
    const int priority = level == LogLevel::kError ? ANDROID_LOG_ERROR
                       : level == LogLevel::kWarn ? ANDROID_LOG_WARN
                       : level == LogLevel::kDebug ? ANDROID_LOG_DEBUG : ANDROID_LOG_INFO;
    __android_log_print(priority, tag, "%s", message);
#else
    std::fprintf(stderr, "[%s] %s\n", tag, message);
#endif
}

}  // namespace platform
}  // namespace asternet

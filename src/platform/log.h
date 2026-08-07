/*
 * AsterNet 网络核心 —— 平台适配：日志
 *
 * 核心内部日志经 asternet_set_log_callback 注册的回调输出。
 */
#ifndef ASTERNET_LOG_H
#define ASTERNET_LOG_H

#include "asternet/asternet.h"

namespace asternet {
namespace platform {

enum class LogLevel {
    kOff = 0,
    kError = 1,
    kWarn = 2,
    kInfo = 3,
    kDebug = 4,
};

void set_log_callback(asternet_log_callback_t callback, void *user_data, int level);

// 核心内部使用
void log(LogLevel level, const char *tag, const char *fmt, ...);

}  // namespace platform
}  // namespace asternet

#define ASTER_LOG_WARN(tag, fmt, ...) \
    ::asternet::platform::log(::asternet::platform::LogLevel::kWarn, tag, fmt, ##__VA_ARGS__)
#define ASTER_LOG_INFO(tag, fmt, ...) \
    ::asternet::platform::log(::asternet::platform::LogLevel::kInfo, tag, fmt, ##__VA_ARGS__)
#define ASTER_LOG_DEBUG(tag, fmt, ...) \
    ::asternet::platform::log(::asternet::platform::LogLevel::kDebug, tag, fmt, ##__VA_ARGS__)

#endif  // ASTERNET_LOG_H

/*
 * AsterNet 网络核心 —— 监控埋点
 *
 * 每次请求采集逐阶段耗时（DNS/建连/TLS/首字节/总耗时）、协议、成功/失败码、
 * 重试/降级路径、迁移事件。跨端口径一致，上报统一大盘。阶段 1 起实现。
 */
#ifndef ASTERNET_METRICS_H
#define ASTERNET_METRICS_H

#include <cstdint>
#include <string>

#include "asternet/asternet.h"  // asternet_response_info_t / asternet_protocol_t

namespace asternet {
namespace monitor {

class MetricsCollector {
public:
    virtual ~MetricsCollector() = default;

    // 上报一次请求的完整指标。在端侧注册的回调线程触发（非网络线程）。
    virtual void report(const asternet_response_info_t &metrics) = 0;
};

}  // namespace monitor
}  // namespace asternet

#endif  // ASTERNET_METRICS_H

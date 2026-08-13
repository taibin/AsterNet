/*
 * AsterNet 网络核心 —— 监控埋点
 *
 * 每次逻辑请求采集逐阶段耗时和策略结果。收集器只保存脱敏聚合数据；上报由
 * 宿主注入，网络核心不直接上传用户数据。
 */
#ifndef ASTERNET_METRICS_H
#define ASTERNET_METRICS_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "asternet/asternet.h"  // asternet_response_info_t / asternet_protocol_t

namespace asternet {
namespace monitor {

struct RequestMetrics {
    asternet_response_info_t response{};
    uint64_t request_id = 0;
    uint64_t network_epoch = 0;
    int attempts = 1;
    bool connection_reused = false;
    bool cache_hit = false;
    bool deduplicated = false;
    std::string failure_stage;
};

class MetricsCollector {
public:
    virtual ~MetricsCollector() = default;

    // 上报一次请求的完整指标。在端侧注册的回调线程触发（非网络线程）。
    virtual void report(const asternet_response_info_t &metrics) = 0;

    virtual void report_request(const RequestMetrics &metrics) { report(metrics.response); }
    virtual std::string dump() const { return "{}"; }
};

class MetricsCollectorImpl final : public MetricsCollector {
public:
    explicit MetricsCollectorImpl(size_t max_events = 128);
    ~MetricsCollectorImpl() override;

    void report(const asternet_response_info_t &metrics) override;
    void report_request(const RequestMetrics &metrics) override;
    std::string dump() const override;
    std::vector<RequestMetrics> recent_events() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace monitor
}  // namespace asternet

#endif  // ASTERNET_METRICS_H

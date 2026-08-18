/*
 * AsterNet 网络核心 —— 监控埋点
 *
 * 每次逻辑请求采集逐阶段耗时和策略结果。收集器只保存脱敏聚合数据；上报由
 * 宿主注入，网络核心不直接上传用户数据。
 */
#ifndef ASTERNET_METRICS_H
#define ASTERNET_METRICS_H

#include <cstdint>
#include <map>
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

struct StageStats {
    // started/succeeded/failed 描述阶段漏斗；samples 描述实际采到耗时的样本数。
    size_t started = 0;
    size_t succeeded = 0;
    size_t failed = 0;
    size_t samples = 0;
    int64_t total_ms = 0;
    int64_t min_ms = -1;
    int64_t max_ms = 0;

    double success_rate() const;
    int64_t average_ms() const;
};

struct MetricsSnapshot {
    size_t events = 0;
    size_t requests = 0;
    size_t success = 0;
    size_t failure = 0;
    size_t degraded = 0;
    size_t deduplicated = 0;
    size_t connection_reused = 0;
    size_t cache_hit = 0;
    size_t attempts = 0;
    int64_t avg_total_ms = -1;
    int64_t max_total_ms = -1;
    StageStats dns;
    StageStats connect;
    StageStats tls;
    StageStats ttfb;
    StageStats transfer;
    StageStats total;
    std::map<std::string, size_t> failure_stages;
};

class MetricsCollector {
public:
    virtual ~MetricsCollector() = default;

    // 上报一次请求的完整指标。自定义实现必须线程安全、快速返回，并避免重入 Client。
    virtual void report(const asternet_response_info_t &metrics) = 0;

    virtual void report_request(const RequestMetrics &metrics) { report(metrics.response); }
    virtual MetricsSnapshot snapshot() const { return {}; }
    virtual std::string dump() const { return "{}"; }
};

class MetricsCollectorImpl final : public MetricsCollector {
public:
    explicit MetricsCollectorImpl(size_t max_events = 128);
    ~MetricsCollectorImpl() override;

    void report(const asternet_response_info_t &metrics) override;
    void report_request(const RequestMetrics &metrics) override;
    MetricsSnapshot snapshot() const override;
    std::string dump() const override;
    std::vector<RequestMetrics> recent_events() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace monitor
}  // namespace asternet

#endif  // ASTERNET_METRICS_H

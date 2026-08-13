/*
 * AsterNet 网络核心 —— 网络探测与弱网判定（SDT）
 *
 * 以真实请求的 RTT、失败率和吞吐为主，主动探测必须由调用方提供受控 endpoint。
 * 不依赖 ICMP，也不把未知数据伪造为中等网络质量。
 */
#ifndef ASTERNET_QUALITY_PROBER_H
#define ASTERNET_QUALITY_PROBER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "asternet/asternet.h"  // asternet_network_t

namespace asternet {
namespace sdt {

struct QualitySample {
    int rtt_ms = -1;         // 往返时延，-1 未知
    int loss_permil = -1;    // 丢包率（千分比），-1 未知
    int bandwidth_kbps = -1; // 带宽估算，-1 未知
    asternet_network_t net = ASTERNET_NETWORK_UNKNOWN;
};

enum class NetworkQuality {
    kUnknown,
    kGood,
    kDegraded,
    kBad,
    kOffline,
};

struct QualitySnapshot {
    int score = -1;  // -1 表示样本不足
    NetworkQuality quality = NetworkQuality::kUnknown;
    size_t samples = 0;
    size_t consecutive_failures = 0;
    size_t total_failures = 0;
    int smoothed_rtt_ms = -1;
    int loss_permil = -1;
    int bandwidth_kbps = -1;
    int64_t last_sample_ms = 0;
    uint64_t network_epoch = 0;
};

// 质量评分：0~100，越低越差；全部未知时返回 -1。
int compute_score(const QualitySample &s);

class QualityProber {
public:
    virtual ~QualityProber() = default;

    // 周期主动探测，更新内部评分
    virtual int probe() = 0;

    // 被动上报业务请求观测数据（成功/失败/RTT），融合进评分
    virtual void observe(bool success, int rtt_ms) = 0;

    virtual int  current_score() const = 0;
    virtual bool is_weak_net() const = 0;

    virtual QualitySnapshot snapshot() const { return {}; }
    virtual void on_network_change(uint64_t /*network_epoch*/, asternet_network_t /*net*/) {}
    virtual std::string dump() const { return "{}"; }
};

class QualityProberImpl final : public QualityProber {
public:
    struct Config {
        int weak_score_threshold = 40;
        int degraded_score_threshold = 65;
        size_t failures_before_bad = 2;
        size_t good_samples_to_recover = 3;
    };

    // probe_callback 必须执行受控、低频的同协议探测，并返回真实结果；为空时 probe()
    // 不产生流量，只返回当前评分。
    using ProbeCallback = std::function<QualitySample()>;

    QualityProberImpl();
    explicit QualityProberImpl(Config config, ProbeCallback probe_callback = {});
    ~QualityProberImpl() override;

    int probe() override;
    void observe(bool success, int rtt_ms) override;
    int current_score() const override;
    bool is_weak_net() const override;
    QualitySnapshot snapshot() const override;
    void on_network_change(uint64_t network_epoch, asternet_network_t net) override;
    std::string dump() const override;

    void observe_sample(bool success, const QualitySample &sample);
    void set_probe_callback(ProbeCallback probe_callback);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace sdt
}  // namespace asternet

#endif  // ASTERNET_QUALITY_PROBER_H

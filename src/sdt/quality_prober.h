/*
 * AsterNet 网络核心 —— 网络探测与弱网判定（SDT）
 *
 * 主动探测 RTT / 丢包 / 带宽，结合被动业务数据，输出网络质量评分。
 * WeakNetDetector 据评分判定弱网；DynamicConfig 据评分动态调整超时/并发/重试。
 * 阶段 1 起实现。
 */
#ifndef ASTERNET_QUALITY_PROBER_H
#define ASTERNET_QUALITY_PROBER_H

#include <cstdint>

#include "asternet/asternet.h"  // asternet_network_t

namespace asternet {
namespace sdt {

struct QualitySample {
    int    rtt_ms;        // 往返时延，-1 未知
    int    loss_permil;   // 丢包率（千分比），-1 未知
    int    bandwidth_kbps;// 带宽估算，-1 未知
    asternet_network_t net;    // 当前网络类型
};

// 质量评分：0~100，越低越差。低于阈值判定弱网。
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
};

}  // namespace sdt
}  // namespace asternet

#endif  // ASTERNET_QUALITY_PROBER_H

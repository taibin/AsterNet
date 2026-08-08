/*
 * AsterNet 网络核心 —— 智能 DNS 接口
 *
 * 解析链路：HttpDNS（主，防劫持）→ LocalDNS（兜底）→ IP 探测测速选优 → 预解析调度 → 缓存。
 * 跨端共享同一选优逻辑，Android/iOS DNS 行为一致。阶段 1 实现。
 */
#ifndef ASTERNET_DNS_RESOLVER_H
#define ASTERNET_DNS_RESOLVER_H

#include <cstdint>
#include <string>
#include <vector>

namespace asternet {
namespace dns {

// 单个解析结果 IP
struct IpResult {
    std::string ip;
    int    rtt_ms;      // 探测 RTT，-1 表示未测
    int    loss_rate;   // 探测丢包率（千分比），-1 表示未测
    int    score;       // 综合评分，越高越优
};

class SmartDnsResolver {
public:
    virtual ~SmartDnsResolver() = default;

    // 异步解析：返回候选 IP 列表（已按 score 排序）。首次用上次最优 IP，探测后更新。
    virtual std::vector<IpResult> resolve(const std::string &host) = 0;

    // 预解析：启动/切前台时对关键域名提前解析并探测，结果入缓存
    virtual int prefetch(const std::string &host) = 0;

    // 清除指定 host 缓存（host 为空则全清）
    virtual void invalidate(const std::string &host = "") = 0;
};

}  // namespace dns
}  // namespace asternet

#endif  // ASTERNET_DNS_RESOLVER_H

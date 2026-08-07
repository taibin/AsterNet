/*
 * AsterNet 网络核心 —— 协议选择器（降级链）
 *
 * 按协议偏好与质量数据选择引擎，失败自动降级：
 *   HTTP/3(QUIC) → HTTP/2 → HTTP/1.1
 *
 * host 级熔断：某协议连续失败 N 次，标记该 host 该协议不可用（冷却期），
 * 冷却后探活恢复。确保可用性底线（最终总能走 HTTP/1.1）。
 *
 * 策略：
 *   1. 按首选协议尝试；失败且可降级 → 降级重试。
 *   2. 已熔断的协议跳过。
 *   3. 降级路径记入 resp，供监控。
 */
#ifndef ASTERNET_PROTOCOL_SELECTOR_H
#define ASTERNET_PROTOCOL_SELECTOR_H

#include <memory>
#include <string>
#include <unordered_map>

#include "engine.h"
#include "asternet/asternet.h"

namespace asternet {
namespace engine {

class ProtocolSelector {
public:
    // 构造：传入已创建的各引擎（nullptr 表示该协议不可用）
    ProtocolSelector(std::shared_ptr<NetworkEngine> h3,
                     std::shared_ptr<NetworkEngine> h2,
                     std::shared_ptr<NetworkEngine> h1);

    // 执行请求：按降级链尝试，返回最终结果。
    // resp.degraded 标记是否发生过降级（通过 err_code 链推断）。
    int request(const Request &req, Response &resp);

    // 按公开协议策略执行请求。ONLY 策略绝不降级，PREFER/AUTO 策略可降级。
    int request_with_policy(const Request &req, asternet_protocol_policy_t policy,
                            Response &resp, asternet_protocol_t *out_actual_proto,
                            bool *out_degraded);

    // 配置
    void set_max_failures(int n) { max_failures_ = n; }       // 连续失败多少次熔断
    void set_cooldown_ms(int64_t ms) { cooldown_ms_ = ms; }   // 熔断冷却时间

private:
    // 某协议是否对该 host 可用（未熔断）
    bool is_available(EngineType t, const std::string &host);
    // 记录一次失败
    void record_failure(EngineType t, const std::string &host);
    // 记录一次成功（清除失败计数）
    void record_success(EngineType t, const std::string &host);

    // host + 引擎类型 → 失败计数与熔断时间
    struct State {
        int failures = 0;
        int64_t banned_until_ms = 0;  // 0 表示未熔断
    };
    using Key = std::pair<std::string, EngineType>;
    struct KeyHash {
        size_t operator()(const Key &k) const noexcept {
            return std::hash<std::string>()(k.first) ^ (size_t)k.second;
        }
    };
    std::unordered_map<Key, State, KeyHash> states_;

    std::shared_ptr<NetworkEngine> h3_;
    std::shared_ptr<NetworkEngine> h2_;
    std::shared_ptr<NetworkEngine> h1_;

    int     max_failures_ = 3;       // 连续失败 3 次熔断
    int64_t cooldown_ms_  = 60000;   // 熔断冷却 60s

    int64_t now_ms() const;
};

}  // namespace engine
}  // namespace asternet

#endif  // ASTERNET_PROTOCOL_SELECTOR_H

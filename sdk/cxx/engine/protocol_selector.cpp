/*
 * AsterNet 网络核心 —— 协议选择器实现（降级链 H3→H2→H1.1）
 */
#include "engine/protocol_selector.h"

#include <time.h>

#include <utility>

#include "platform/log.h"

namespace asternet {
namespace engine {

namespace {

bool is_retryable_error(int error) {
    return error == ASTERNET_ERR_DNS || error == ASTERNET_ERR_CONNECT
        || error == ASTERNET_ERR_TIMEOUT || error == ASTERNET_ERR_PROTOCOL
        || error == ASTERNET_ERR_UNSUPPORTED || error == ASTERNET_ERR_DEGRADED;
}

}  // namespace

ProtocolSelector::ProtocolSelector(std::shared_ptr<NetworkEngine> h3,
                                   std::shared_ptr<NetworkEngine> h2,
                                   std::shared_ptr<NetworkEngine> h1)
    : h3_(std::move(h3)), h2_(std::move(h2)), h1_(std::move(h1)) {}

int64_t ProtocolSelector::now_ms() const {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string ProtocolSelector::state_host_key(const Request &request) const {
    return request.host + '\n' + std::to_string(request.port) + '\n'
        + std::to_string(request.network_epoch);
}

bool ProtocolSelector::is_available(EngineType t, const Request &request) {
    std::lock_guard<std::mutex> lock(states_mutex_);
    const std::string host = state_host_key(request);
    auto it = states_.find({host, t});
    if (it == states_.end()) return true;
    if (it->second.banned_until_ms == 0) return true;
    // 冷却期过 → 解禁，重置计数
    if (now_ms() >= it->second.banned_until_ms) {
        it->second.banned_until_ms = 0;
        it->second.failures = 0;
        return true;
    }
    return false;
}

void ProtocolSelector::record_failure(EngineType t, const Request &request) {
    std::lock_guard<std::mutex> lock(states_mutex_);
    const std::string host = state_host_key(request);
    if (states_.size() >= max_states_ && states_.find({host, t}) == states_.end()) {
        states_.erase(states_.begin());
    }
    auto &s = states_[{host, t}];
    if (++s.failures >= max_failures_) {
        s.banned_until_ms = now_ms() + cooldown_ms_;
    }
}

void ProtocolSelector::record_success(EngineType t, const Request &request) {
    std::lock_guard<std::mutex> lock(states_mutex_);
    const std::string host = state_host_key(request);
    auto it = states_.find({host, t});
    if (it != states_.end()) {
        it->second.failures = 0;
        it->second.banned_until_ms = 0;
    }
}

int ProtocolSelector::request(const Request &req, Response &resp) {
    return request_with_policy(req, ASTERNET_POLICY_AUTO, resp, nullptr, nullptr);
}

int ProtocolSelector::request_with_policy(const Request &req, asternet_protocol_policy_t policy,
                                          Response &resp, asternet_protocol_t *out_actual_proto,
                                          bool *out_degraded) {
    if (out_actual_proto) *out_actual_proto = ASTERNET_PROTOCOL_UNKNOWN;
    if (out_degraded) *out_degraded = false;

    struct Step {
        EngineType type;
        std::shared_ptr<NetworkEngine> *engine;
        asternet_protocol_t proto;
        const char *name;
    };
    const Step h3_step{EngineType::kHttp3, &h3_, ASTERNET_PROTOCOL_HTTP_3, "H3"};
    const Step h2_step{EngineType::kHttp2, &h2_, ASTERNET_PROTOCOL_HTTP_2, "H2"};
    const Step h1_step{EngineType::kHttp1, &h1_, ASTERNET_PROTOCOL_HTTP_1_1, "H1"};
    const Step auto_chain[] = {h3_step, h2_step, h1_step};
    const Step h2_chain[] = {h2_step, h1_step};

    const Step *chain = nullptr;
    size_t chain_size = 0;
    bool allow_fallback = true;
    bool respect_circuit = true;
    switch (policy) {
    case ASTERNET_POLICY_AUTO:
    case ASTERNET_POLICY_PREFER_HTTP_3:
        chain = auto_chain;
        chain_size = sizeof(auto_chain) / sizeof(auto_chain[0]);
        break;
    case ASTERNET_POLICY_PREFER_HTTP_2:
        chain = h2_chain;
        chain_size = sizeof(h2_chain) / sizeof(h2_chain[0]);
        break;
    case ASTERNET_POLICY_HTTP_3_ONLY:
        chain = &h3_step;
        chain_size = 1;
        allow_fallback = false;
        respect_circuit = false;
        break;
    case ASTERNET_POLICY_HTTP_2_ONLY:
        chain = &h2_step;
        chain_size = 1;
        allow_fallback = false;
        respect_circuit = false;
        break;
    case ASTERNET_POLICY_HTTP_1_1_ONLY:
        chain = &h1_step;
        chain_size = 1;
        allow_fallback = false;
        respect_circuit = false;
        break;
    default:
        resp.err_code = ASTERNET_ERR_INVALID_ARGUMENT;
        return ASTERNET_ERR_INVALID_ARGUMENT;
    }

    ASTER_LOG_INFO("asternet-selector", "==> %s:%d %s %s policy=%d",
               req.host.c_str(), req.port, req.method.c_str(), req.path.c_str(),
               static_cast<int>(policy));

    int last_error = ASTERNET_ERR_UNSUPPORTED;
    bool degraded = false;
    int attempts = 0;
    const int64_t request_start_ms = now_ms();
    for (size_t i = 0; i < chain_size; ++i) {
        const Step &step = chain[i];
        if (*step.engine == nullptr) {
            ASTER_LOG_INFO("asternet-selector", "  skip %s (not built)", step.name);
            last_error = ASTERNET_ERR_UNSUPPORTED;
            if (!allow_fallback) break;
            continue;
        }
        if (respect_circuit && !is_available(step.type, req)) {
            ASTER_LOG_WARN("asternet-selector", "  skip %s (circuit open for %s)",
                       step.name, req.host.c_str());
            last_error = ASTERNET_ERR_DEGRADED;
            if (!allow_fallback) break;
            degraded = true;
            continue;
        }

        ASTER_LOG_INFO("asternet-selector", "  try %s", step.name);
        const int64_t elapsed_ms = now_ms() - request_start_ms;
        const int configured_timeout_ms = req.timeout_ms > 0 ? req.timeout_ms : 15000;
        const int64_t remaining_ms = static_cast<int64_t>(configured_timeout_ms) - elapsed_ms;
        if (remaining_ms <= 0) {
            last_error = ASTERNET_ERR_TIMEOUT;
            break;
        }
        Request attempt = req;
        attempt.timeout_ms = static_cast<int>(remaining_ms);
        Response tmp;
        ++attempts;
        const int ret = (*step.engine)->request(attempt, tmp);
        if (ret == ASTERNET_OK) {
            record_success(step.type, req);
            resp = std::move(tmp);
            resp.protocol = step.proto;
            resp.degraded = degraded;
            resp.attempts = attempts;
            if (out_actual_proto) *out_actual_proto = step.proto;
            if (out_degraded) *out_degraded = degraded;
            ASTER_LOG_INFO("asternet-selector", "  %s OK status=%d body_len=%zu degraded=%d",
                       step.name, resp.http_status, resp.body.size(), degraded);
            return ASTERNET_OK;
        }

        resp = std::move(tmp);
        last_error = ret;
        ASTER_LOG_WARN("asternet-selector", "  %s FAIL ret=%d", step.name, ret);

        if (!is_retryable_error(ret)) break;
        record_failure(step.type, req);

        // Retrying a non-idempotent request could duplicate an accepted write.
        if (!allow_fallback || !req.retry_safe) break;
        degraded = true;
    };

    if (resp.err_code == ASTERNET_OK) resp.err_code = last_error;
    resp.attempts = attempts;
    if (out_degraded) *out_degraded = degraded;
    ASTER_LOG_WARN("asternet-selector", "<== request failed ret=%d degraded=%d", resp.err_code, degraded);
    return resp.err_code;
}

}  // namespace engine
}  // namespace asternet

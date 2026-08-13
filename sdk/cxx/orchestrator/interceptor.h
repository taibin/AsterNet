/*
 * AsterNet 网络核心 —— 请求编排接口
 *
 * 拦截器链负责请求级策略；ProtocolSelector 独占 H3→H2→H1 的协议降级，避免
 * 重试层和协议层重复放大请求。in-flight 合并仅适用于无认证状态的安全 GET/HEAD。
 */
#ifndef ASTERNET_INTERCEPTOR_H
#define ASTERNET_INTERCEPTOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "asternet/asternet.h"
#include "engine/engine.h"
#include "sdt/quality_prober.h"

namespace asternet {
namespace orchestrator {

struct RequestContext {
    engine::Request request;
    asternet_protocol_policy_t policy = ASTERNET_POLICY_AUTO;
    uint64_t request_id = 0;
    uint64_t network_epoch = 0;
    int attempts = 0;
    int max_retries = 1;
    int64_t deadline_ms = 0;
    bool weak_network = false;
    bool deduplicated = false;
    bool dns_cache_hit = false;
};

using Terminal = std::function<int(RequestContext &, engine::Response &)>;

class Chain;

// 拦截器接口（链式）。intercept 调用 chain.proceed() 推进下一环。
class Interceptor {
public:
    virtual ~Interceptor() = default;
    virtual int intercept(Chain &chain, RequestContext &context, engine::Response &response) = 0;
};

class Chain {
public:
    int proceed(RequestContext &context, engine::Response &response);

private:
    friend class InterceptorChain;
    Chain(const std::vector<std::unique_ptr<Interceptor>> &interceptors, size_t index,
          Terminal terminal)
        : interceptors_(interceptors), index_(index), terminal_(std::move(terminal)) {}

    const std::vector<std::unique_ptr<Interceptor>> &interceptors_;
    size_t index_;
    Terminal terminal_;
};

class InterceptorChain final {
public:
    void add(std::unique_ptr<Interceptor> interceptor);
    int execute(RequestContext &context, engine::Response &response, Terminal terminal) const;

private:
    std::vector<std::unique_ptr<Interceptor>> interceptors_;
};

class RetryInterceptor final : public Interceptor {
public:
    int intercept(Chain &chain, RequestContext &context, engine::Response &response) override;

private:
    static bool is_retryable(int result);
};

class WeakNetInterceptor final : public Interceptor {
public:
    explicit WeakNetInterceptor(std::shared_ptr<sdt::QualityProber> prober)
        : prober_(std::move(prober)) {}

    int intercept(Chain &chain, RequestContext &context, engine::Response &response) override;

private:
    std::shared_ptr<sdt::QualityProber> prober_;
};

class RequestCoalescer final {
public:
    explicit RequestCoalescer(size_t max_flights = 128);
    ~RequestCoalescer();
    RequestCoalescer(const RequestCoalescer &) = delete;
    RequestCoalescer &operator=(const RequestCoalescer &) = delete;

    int execute(RequestContext &context, engine::Response &response, const Terminal &operation);

private:
    struct State;
    std::unique_ptr<State> state_;
};

class RequestOrchestrator final {
public:
    explicit RequestOrchestrator(std::shared_ptr<sdt::QualityProber> prober);

    int execute(RequestContext &context, engine::Response &response, Terminal terminal);

private:
    InterceptorChain chain_;
    RequestCoalescer coalescer_;
};

}  // namespace orchestrator
}  // namespace asternet

#endif  // ASTERNET_INTERCEPTOR_H

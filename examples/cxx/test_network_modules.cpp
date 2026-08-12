#include "connection/connection_pool.h"
#include "dns/dns_resolver.h"
#include "engine/protocol_selector.h"
#include "monitor/metrics.h"
#include "orchestrator/interceptor.h"
#include "protocol/protocol.h"
#include "sdt/quality_prober.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

class FakeEngine final : public asternet::engine::NetworkEngine {
public:
    FakeEngine(asternet::engine::EngineType type, int result)
        : type_(type), result_(result) {}

    asternet::engine::EngineType type() const override { return type_; }
    asternet::engine::EngineCaps caps() const override { return {}; }
    int request(const asternet::engine::Request &, asternet::engine::Response &response) override {
        ++requests;
        response.err_code = result_;
        response.protocol = type_ == asternet::engine::EngineType::kHttp3
            ? ASTERNET_PROTOCOL_HTTP_3
            : type_ == asternet::engine::EngineType::kHttp2
                ? ASTERNET_PROTOCOL_HTTP_2 : ASTERNET_PROTOCOL_HTTP_1_1;
        if (result_ == ASTERNET_OK) response.http_status = 200;
        return result_;
    }

    std::atomic<int> requests{0};

private:
    asternet::engine::EngineType type_;
    int result_;
};

void test_dns() {
    using asternet::dns::IpResult;
    using asternet::dns::SmartDnsResolverImpl;
    SmartDnsResolverImpl resolver({}, [](const std::string &) {
        return std::vector<IpResult>{{"192.0.2.10"}, {"2001:db8::10"}};
    });
    auto first = resolver.resolve_with_metadata("api.example.test", 1);
    assert(first.error == ASTERNET_OK);
    assert(first.addresses.size() == 2);
    assert(!first.cache_hit);
    auto cached = resolver.resolve_with_metadata("api.example.test", 1);
    assert(cached.cache_hit);
    resolver.report_connection_result("api.example.test", "192.0.2.10", 1, false, -1);
    resolver.invalidate("api.example.test");
    auto reordered = resolver.resolve_with_metadata("api.example.test", 1);
    assert(reordered.addresses.size() == 2);
    assert(reordered.addresses.front().ip != "192.0.2.10");
}

void test_connection_pool() {
    asternet::connection::ConnectionPoolImpl pool(2);
    asternet::connection::Origin origin;
    origin.host = "api.example.test";
    origin.protocol = ASTERNET_PROTOCOL_HTTP_2;
    auto lease = pool.acquire(origin);
    assert(lease.valid);
    pool.release(lease, true);
    const auto snapshot = pool.snapshot();
    assert(snapshot.origins == 1);
    assert(snapshot.active_leases == 0);
    assert(pool.prefetch(origin.host) == ASTERNET_ERR_UNSUPPORTED);
    pool.on_network_change(2, ASTERNET_NETWORK_WIFI);
    assert(pool.snapshot().origins == 0);
}

void test_quality() {
    assert(asternet::sdt::compute_score({}) == -1);
    asternet::sdt::QualityProberImpl prober;
    prober.observe(true, 80);
    assert(prober.current_score() >= 0);
    prober.observe(false, -1);
    prober.observe(false, -1);
    assert(prober.is_weak_net());
    prober.on_network_change(7, ASTERNET_NETWORK_WIFI);
    assert(prober.snapshot().quality == asternet::sdt::NetworkQuality::kUnknown);
}

void test_protocol_selector() {
    auto h1 = std::make_shared<FakeEngine>(asternet::engine::EngineType::kHttp1, ASTERNET_OK);
    asternet::engine::ProtocolSelector selector(nullptr, nullptr, h1);
    asternet::engine::Request request;
    request.host = "api.example.test";
    request.idempotent = true;
    request.retry_safe = true;
    request.timeout_ms = 1000;
    asternet::engine::Response response;
    bool degraded = true;
    asternet_protocol_t protocol = ASTERNET_PROTOCOL_UNKNOWN;
    assert(selector.request_with_policy(request, ASTERNET_POLICY_AUTO, response, &protocol, &degraded)
           == ASTERNET_OK);
    assert(protocol == ASTERNET_PROTOCOL_HTTP_1_1);
    assert(!degraded);
    assert(h1->requests == 1);

    auto h3 = std::make_shared<FakeEngine>(asternet::engine::EngineType::kHttp3,
                                           ASTERNET_ERR_PROTOCOL);
    asternet::engine::ProtocolSelector h3_selector(h3, nullptr, h1);
    h3_selector.set_max_failures(1);
    asternet::engine::Response failed_response;
    assert(h3_selector.request_with_policy(request, ASTERNET_POLICY_AUTO, failed_response,
                                           nullptr, nullptr) == ASTERNET_OK);
    assert(h3->requests == 1);
    asternet::engine::Response skipped_response;
    assert(h3_selector.request_with_policy(request, ASTERNET_POLICY_AUTO, skipped_response,
                                           nullptr, nullptr) == ASTERNET_OK);
    assert(h3->requests == 1);
    asternet::engine::Response only_response;
    assert(h3_selector.request_with_policy(request, ASTERNET_POLICY_HTTP_3_ONLY, only_response,
                                           nullptr, nullptr) == ASTERNET_ERR_PROTOCOL);
    assert(h3->requests == 2);
}

void test_orchestrator_and_metrics() {
    auto prober = std::make_shared<asternet::sdt::QualityProberImpl>();
    asternet::orchestrator::RequestOrchestrator orchestrator(prober);
    asternet::orchestrator::RequestContext context;
    context.request.host = "api.example.test";
    context.request.method = "GET";
    context.request.idempotent = true;
    context.request.timeout_ms = 2000;
    context.request_id = 42;
    asternet::engine::Response response;
    int calls = 0;
    assert(orchestrator.execute(context, response,
        [&calls](asternet::orchestrator::RequestContext &, asternet::engine::Response &result) -> int {
            ++calls;
            if (calls == 1) {
                result.err_code = ASTERNET_ERR_CONNECT;
                return result.err_code;
            }
            result.err_code = ASTERNET_OK;
            result.http_status = 200;
            return static_cast<int>(ASTERNET_OK);
        }) == ASTERNET_OK);
    assert(calls == 2);
    assert(response.attempts == 2);

    asternet::orchestrator::RequestContext post_context;
    post_context.request.host = "api.example.test";
    post_context.request.method = "POST";
    post_context.request.idempotent = true;
    post_context.request.timeout_ms = 2000;
    post_context.request.headers.push_back({"Idempotency-Key", "abc123"});
    asternet::engine::Response post_response;
    int post_calls = 0;
    assert(orchestrator.execute(post_context, post_response,
        [&post_calls](asternet::orchestrator::RequestContext &, asternet::engine::Response &result) -> int {
            ++post_calls;
            if (post_calls == 1) {
                result.err_code = ASTERNET_ERR_CONNECT;
                return result.err_code;
            }
            result.err_code = ASTERNET_OK;
            result.http_status = 201;
            return static_cast<int>(ASTERNET_OK);
        }) == ASTERNET_OK);
    assert(post_calls == 2);

    asternet::orchestrator::RequestContext blank_key_context;
    blank_key_context.request.host = "api.example.test";
    blank_key_context.request.method = "POST";
    blank_key_context.request.idempotent = true;
    blank_key_context.request.timeout_ms = 2000;
    blank_key_context.request.headers.push_back({"Idempotency-Key", "   "});
    asternet::engine::Response blank_key_response;
    int blank_key_calls = 0;
    assert(orchestrator.execute(blank_key_context, blank_key_response,
        [&blank_key_calls](asternet::orchestrator::RequestContext &, asternet::engine::Response &result) -> int {
            ++blank_key_calls;
            result.err_code = ASTERNET_ERR_CONNECT;
            return result.err_code;
        }) == ASTERNET_ERR_CONNECT);
    assert(blank_key_calls == 1);

    asternet::orchestrator::RequestContext unsafe_context;
    unsafe_context.request.host = "api.example.test";
    unsafe_context.request.method = "POST";
    unsafe_context.request.idempotent = true;
    unsafe_context.request.timeout_ms = 2000;
    unsafe_context.request.headers.push_back({"Idempotency-Key", ""});
    asternet::engine::Response unsafe_response;
    int unsafe_calls = 0;
    assert(orchestrator.execute(unsafe_context, unsafe_response,
        [&unsafe_calls](asternet::orchestrator::RequestContext &, asternet::engine::Response &result) -> int {
            ++unsafe_calls;
            result.err_code = ASTERNET_ERR_CONNECT;
            return result.err_code;
        }) == ASTERNET_ERR_CONNECT);
    assert(unsafe_calls == 1);

    asternet::orchestrator::RequestContext body_context;
    body_context.request.host = "api.example.test";
    body_context.request.method = "GET";
    body_context.request.idempotent = true;
    body_context.request.timeout_ms = 2000;
    body_context.request.body = "payload";
    asternet::engine::Response body_response;
    int body_calls = 0;
    assert(orchestrator.execute(body_context, body_response,
        [&body_calls](asternet::orchestrator::RequestContext &, asternet::engine::Response &result) -> int {
            ++body_calls;
            result.err_code = ASTERNET_ERR_CONNECT;
            return result.err_code;
        }) == ASTERNET_ERR_CONNECT);
    assert(body_calls == 1);

    asternet::monitor::MetricsCollectorImpl collector(2);
    asternet_response_info_t info{};
    info.result = ASTERNET_OK;
    collector.report(info);
    assert(collector.recent_events().size() == 1);
}

void test_protocol_codec() {
    asternet::protocol::LengthPrefixedCodec codec(16);
    std::vector<uint8_t> wire;
    assert(codec.encode("hello", wire) == ASTERNET_OK);
    std::string decoded;
    assert(codec.decode(wire.data(), wire.size(), decoded) == ASTERNET_OK);
    assert(decoded == "hello");
    assert(codec.decode(wire.data(), wire.size() - 1, decoded) == ASTERNET_ERR_PROTOCOL);
}

}  // namespace

int main() {
    test_dns();
    test_connection_pool();
    test_quality();
    test_protocol_selector();
    test_orchestrator_and_metrics();
    test_protocol_codec();
    std::puts("test_network_modules OK");
    return 0;
}

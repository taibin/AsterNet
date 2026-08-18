#include "client.h"
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
#include <string>
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

class StubMetricsCollector final : public asternet::monitor::MetricsCollector {
public:
    void report(const asternet_response_info_t &) override {}
    std::string dump() const override { return "{\"custom_metrics\":true}"; }
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
    auto literal = resolver.resolve_with_metadata("203.0.113.10", 1);
    assert(literal.error == ASTERNET_OK);
    assert(literal.addresses.size() == 1);

    auto loopback = resolver.resolve_with_metadata("::ffff:127.0.0.1", 1);
    assert(loopback.error == ASTERNET_ERR_DNS);
    auto nat64_loopback = resolver.resolve_with_metadata("64:ff9b::7f00:1", 1);
    assert(nat64_loopback.error == ASTERNET_ERR_DNS);
    auto nat64_local_prefix = resolver.resolve_with_metadata("64:ff9b:1:0a00:0001::", 1);
    assert(nat64_local_prefix.error == ASTERNET_ERR_DNS);
    auto translated_private = resolver.resolve_with_metadata("::ffff:0:0a00:1", 1);
    assert(translated_private.error == ASTERNET_ERR_DNS);
    auto six_to_four_private = resolver.resolve_with_metadata("2002:0a00:0001::1", 1);
    assert(six_to_four_private.error == ASTERNET_ERR_DNS);
    auto compatible_loopback = resolver.resolve_with_metadata("::127.0.0.1", 1);
    assert(compatible_loopback.error == ASTERNET_ERR_DNS);
    auto compatible_private = resolver.resolve_with_metadata("::10.0.0.1", 1);
    assert(compatible_private.error == ASTERNET_ERR_DNS);

    SmartDnsResolverImpl fallback_resolver;
    fallback_resolver.set_backup_ips("blocked.example.test", {{"10.0.0.1"}, {"203.0.113.20"}});
    auto fallback = fallback_resolver.resolve_with_metadata("blocked.example.test", 1, 1);
    assert(fallback.error == ASTERNET_OK);
    assert(fallback.addresses.size() == 1);
    assert(fallback.addresses.front().ip == "203.0.113.20");
}

void test_connection_pool() {
    asternet::connection::ConnectionPoolImpl pool(2);
    int prefetch_calls = 0;
    int migration_calls = 0;
    pool.set_prefetch_handler([&prefetch_calls](const std::string &) {
        ++prefetch_calls;
        return ASTERNET_OK;
    });
    pool.set_migration_handler([&migration_calls] {
        ++migration_calls;
        return ASTERNET_ERR_UNSUPPORTED;
    });
    asternet::connection::Origin origin;
    origin.host = "api.example.test";
    origin.protocol = ASTERNET_PROTOCOL_HTTP_2;
    auto lease = pool.acquire(origin);
    assert(lease.valid);
    pool.release(lease, true);
    const auto snapshot = pool.snapshot();
    assert(snapshot.origins == 1);
    assert(snapshot.active_leases == 0);
    assert(pool.prefetch(origin.host) == ASTERNET_OK);
    assert(prefetch_calls == 1);
    pool.on_network_change(2, ASTERNET_NETWORK_WIFI);
    assert(pool.migrate(ASTERNET_NETWORK_WIFI) == ASTERNET_ERR_UNSUPPORTED);
    const auto changed = pool.snapshot();
    assert(changed.origins == 0);
    assert(changed.evictions >= 1);
    assert(changed.migrations == 1);
    assert(migration_calls == 1);
}

void test_quality() {
    assert(asternet::sdt::compute_score({}) == -1);
    asternet::sdt::QualityProberImpl prober;
    prober.observe(true, 80);
    assert(prober.current_score() >= 0);
    prober.observe(false, -1);
    prober.observe(false, -1);
    assert(prober.is_weak_net());
    assert(prober.snapshot().loss_permil > 0);
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
    info.total_ms = 12;
    collector.report(info);
    assert(collector.recent_events().size() == 1);
    const std::string metrics_dump = collector.dump();
    assert(metrics_dump.find("\"stages\"") != std::string::npos);
    assert(metrics_dump.find("\"dns\"") != std::string::npos);
    assert(metrics_dump.find("\"success_rate\"") != std::string::npos);

    asternet::monitor::MetricsCollectorImpl detailed_collector(4);
    asternet::monitor::RequestMetrics h3_metrics;
    h3_metrics.response.result = ASTERNET_OK;
    h3_metrics.response.protocol = ASTERNET_PROTOCOL_HTTP_3;
    h3_metrics.response.dns_ms = 1;
    h3_metrics.response.connect_ms = 2;
    h3_metrics.response.tls_ms = 5;
    h3_metrics.response.ttfb_ms = 8;
    h3_metrics.response.total_ms = 20;
    h3_metrics.attempts = 2;
    h3_metrics.connection_reused = true;
    h3_metrics.cache_hit = true;
    detailed_collector.report_request(h3_metrics);
    const asternet::monitor::MetricsSnapshot h3_snapshot = detailed_collector.snapshot();
    assert(h3_snapshot.requests == 1);
    assert(h3_snapshot.attempts == 2);
    assert(h3_snapshot.tls.started == 1);
    assert(h3_snapshot.tls.succeeded == 1);
    assert(h3_snapshot.ttfb.started == 1);
    assert(h3_snapshot.ttfb.succeeded == 1);
    assert(h3_snapshot.transfer.average_ms() == 12);

    asternet::monitor::RequestMetrics failure_metrics;
    failure_metrics.response.result = ASTERNET_ERR_PROTOCOL;
    failure_metrics.response.dns_ms = -1;
    failure_metrics.response.connect_ms = -1;
    failure_metrics.response.tls_ms = -1;
    failure_metrics.response.ttfb_ms = -1;
    failure_metrics.response.total_ms = 3;
    failure_metrics.failure_stage = "body";
    failure_metrics.failure_stage.push_back('\x01');
    detailed_collector.report_request(failure_metrics);
    assert(detailed_collector.dump().find("\\u0001") != std::string::npos);

    asternet::Client client({});
    assert(client.set_metrics_collector(std::make_shared<StubMetricsCollector>()) == ASTERNET_OK);
    assert(client.dump_diag().find("\"metrics\"") != std::string::npos);
}

void test_protocol_codec() {
    asternet::protocol::LengthPrefixedCodec codec(16);
    std::vector<uint8_t> wire;
    assert(codec.encode("hello", wire) == ASTERNET_OK);
    std::string decoded;
    assert(codec.decode(wire.data(), wire.size(), decoded) == ASTERNET_OK);
    assert(decoded == "hello");
    assert(codec.decode(wire.data(), wire.size() - 1, decoded) == ASTERNET_ERR_PROTOCOL);

    asternet::protocol::GatewayRequest gateway;
    gateway.host = "api.example.test";
    gateway.method = "POST";
    gateway.path = "/gateway";
    gateway.body = "{}";
    gateway.headers.push_back({"content-type", "application/json"});
    asternet::engine::Request request;
    asternet::protocol::DefaultGatewayProtocol protocol;
    assert(protocol.adapt(gateway, request) == ASTERNET_OK);
    assert(request.host == gateway.host);
    assert(request.method == gateway.method);
    assert(request.path == gateway.path);
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

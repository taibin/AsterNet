/*
 * AsterNet 网络核心 —— C++ 内部门面实现（阶段 0 最小骨架）
 */
#include "client.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>
#include <sstream>

#include "asternet/version.h"
#include "platform/log.h"

namespace asternet {

namespace {

constexpr size_t kMaxActiveClients = 32;
thread_local Client *g_active_clients[kMaxActiveClients]{};
thread_local size_t g_active_client_count = 0;

bool is_active_client(Client *client) {
    for (size_t i = 0; i < g_active_client_count; ++i) {
        if (g_active_clients[i] == client) return true;
    }
    return false;
}

class ActiveRequestGuard {
public:
    explicit ActiveRequestGuard(Client *client) : installed_(g_active_client_count < kMaxActiveClients) {
        if (installed_) g_active_clients[g_active_client_count++] = client;
    }
    ~ActiveRequestGuard() {
        if (installed_) --g_active_client_count;
    }

private:
    bool installed_;
};

int64_t monotonic_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool is_replayable_safe_method(const engine::Request &request) {
    const std::string &method = request.method;
    if (!request.body.empty()) return false;
    return method == "GET" || method == "HEAD" || method == "OPTIONS";
}

std::shared_ptr<monitor::MetricsCollector> make_default_metrics_collector() {
    return std::make_shared<monitor::MetricsCollectorImpl>();
}

}  // namespace

bool Client::check_abi(uint32_t abi_version) {
    const uint32_t expected_major = ASTERNET_ABI_VERSION >> 16;
    const uint32_t got_major = abi_version >> 16;
    const uint32_t expected_minor = ASTERNET_ABI_VERSION & 0xffffu;
    const uint32_t got_minor = abi_version & 0xffffu;
    return got_major == expected_major && got_minor <= expected_minor;
}

Client::Client(const asternet_client_config_t &cfg)
    : config_(cfg), ca_cert_pem_(cfg.ca_cert_pem != nullptr ? cfg.ca_cert_pem : "") {
    // 构造三引擎 + 协议选择器（降级链 H3→H2→H1.1）
    // ABI keeps the deprecated test-only field for layout compatibility, but production core never
    // disables certificate or hostname verification.
    const bool allow_insecure = false;
    h1_engine_ = std::make_shared<engine::Http1Engine>(allow_insecure, ca_cert_pem_);
    h2_engine_ = std::make_shared<engine::Http2Engine>(allow_insecure, ca_cert_pem_);
#ifdef ASTERNET_ENABLE_XQUIC
    if (config_.enable_http3 != 0) {
        h3_engine_ = std::make_shared<engine::QuicEngine>(allow_insecure, ca_cert_pem_);
    }
#endif
    selector_ = std::make_unique<engine::ProtocolSelector>(h3_engine_, h2_engine_, h1_engine_);
    dns::SmartDnsResolverImpl::Config dns_config;
    dns_config.allow_private_addresses = config_.allow_private_networks != 0;
    dns_resolver_ = std::make_shared<dns::SmartDnsResolverImpl>(dns_config);
    auto pool = std::make_shared<connection::ConnectionPoolImpl>();
    std::weak_ptr<engine::NetworkEngine> h1 = h1_engine_;
    std::weak_ptr<engine::NetworkEngine> h2 = h2_engine_;
    std::weak_ptr<engine::NetworkEngine> h3 = h3_engine_;
    pool->set_prefetch_handler([h3](const std::string &host) {
        const std::shared_ptr<engine::NetworkEngine> engine = h3.lock();
        return engine ? engine->prefetch(host) : ASTERNET_ERR_UNSUPPORTED;
    });
    pool->set_migration_handler([h1, h2, h3] {
        int result = ASTERNET_ERR_UNSUPPORTED;
        if (const std::shared_ptr<engine::NetworkEngine> engine = h1.lock()) {
            result = engine->migrate_connection();
        }
        if (const std::shared_ptr<engine::NetworkEngine> engine = h2.lock()) {
            const int current = engine->migrate_connection();
            if (result != ASTERNET_OK) result = current;
        }
        if (const std::shared_ptr<engine::NetworkEngine> engine = h3.lock()) {
            const int current = engine->migrate_connection();
            if (result != ASTERNET_OK) result = current;
        }
        return result;
    });
    connection_pool_ = std::move(pool);
    quality_prober_ = std::make_shared<sdt::QualityProberImpl>();
    orchestrator_ = std::make_unique<orchestrator::RequestOrchestrator>(quality_prober_);
    metrics_shadow_collector_ = make_default_metrics_collector();
    metrics_collector_ = metrics_shadow_collector_;
    metrics_worker_thread_ = std::thread([this] { metrics_worker_loop(); });
    gateway_protocol_ = std::make_unique<protocol::DefaultGatewayProtocol>();
}

Client::~Client() {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    destroyed_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> queue_lock(metrics_queue_mutex_);
        metrics_worker_stop_ = true;
    }
    metrics_queue_cv_.notify_all();
    if (metrics_worker_thread_.joinable()) {
        lock.unlock();
        metrics_worker_thread_.join();
        lock.lock();
    }
    if (connection_pool_) connection_pool_->evict_all();
    gateway_protocol_.reset();
    metrics_collector_.reset();
    metrics_shadow_collector_.reset();
    orchestrator_.reset();
    quality_prober_.reset();
    connection_pool_.reset();
    dns_resolver_.reset();
    selector_.reset();
    h3_engine_.reset();
    h2_engine_.reset();
    h1_engine_.reset();
}

void Client::on_network_change(asternet_network_t net) {
    if (is_active_client(this)) return;
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (destroyed_.load(std::memory_order_acquire)) return;
    const uint64_t epoch = network_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (connection_pool_) {
        connection_pool_->on_network_change(epoch, net);
        // This returns UNSUPPORTED until the H3 engine owns persistent connections. Cache and
        // lease invalidation above is still required for every transport.
        (void)connection_pool_->migrate(net);
    }
    if (dns_resolver_) dns_resolver_->on_network_change(epoch);
    if (quality_prober_) quality_prober_->on_network_change(epoch, net);
}

std::string Client::dump_diag() const {
    if (is_active_client(const_cast<Client *>(this))) return "{}";
    std::string dns_dump;
    std::string connections_dump;
    std::string quality_dump;
    uint64_t epoch = 0;
    {
        std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
        epoch = network_epoch_.load(std::memory_order_acquire);
        dns_dump = dns_resolver_ ? dns_resolver_->dump() : "{}";
        connections_dump = connection_pool_ ? connection_pool_->dump() : "{}";
        quality_dump = quality_prober_ ? quality_prober_->dump() : "{}";
    }
    std::string metrics_dump = "{}";
    if (metrics_shadow_collector_) {
        try {
            metrics_dump = metrics_shadow_collector_->dump();
        } catch (...) {
            ASTER_LOG_WARN("asternet-client", "metrics dump failed");
        }
    }
    std::ostringstream out;
    out << "{\"network_epoch\":" << epoch
        << ",\"dns\":" << dns_dump
        << ",\"connections\":" << connections_dump
        << ",\"quality\":" << quality_dump
        << ",\"metrics\":" << metrics_dump
        << "}";
    return out.str();
}

int Client::prefetch(const std::string &host) {
    if (host.empty()) return ASTERNET_ERR_INVALID_ARGUMENT;
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (destroyed_.load(std::memory_order_acquire)) return ASTERNET_ERR_CANCELED;
    int dns_result = ASTERNET_OK;
    if (dns_resolver_) dns_result = dns_resolver_->prefetch(
        host, network_epoch_.load(std::memory_order_acquire));
    if (connection_pool_) (void)connection_pool_->prefetch(host);
    return dns_result;
}

int Client::set_metrics_collector(std::shared_ptr<monitor::MetricsCollector> collector) {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (destroyed_.load(std::memory_order_acquire)) return ASTERNET_ERR_CANCELED;
    if (collector == nullptr) {
        metrics_collector_ = metrics_shadow_collector_;
    } else {
        metrics_collector_ = std::move(collector);
    }
    return ASTERNET_OK;
}

int Client::request(const engine::Request &req, engine::Response &resp) {
    return request_with_policy(req, ASTERNET_POLICY_AUTO, resp, nullptr, nullptr);
}

int Client::request_with_policy(const engine::Request &req, asternet_protocol_policy_t policy,
                                engine::Response &resp, asternet_protocol_t *out_actual_proto,
                                bool *out_degraded) {
    if (is_active_client(this) || g_active_client_count >= kMaxActiveClients) {
        return ASTERNET_ERR_INTERNAL;
    }
    ActiveRequestGuard active_guard(this);
    const int64_t started_ms = monotonic_ms();
    orchestrator::RequestContext context;
    std::shared_ptr<monitor::MetricsCollector> metrics_collector;
    int ret = ASTERNET_ERR_INTERNAL;
    {
        std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
        if (destroyed_.load(std::memory_order_acquire)) return ASTERNET_ERR_CANCELED;
        if (!selector_ || !orchestrator_) return ASTERNET_ERR_INTERNAL;

        context.request = req;
        context.request.allow_insecure_tls_for_testing = false;
        context.request.timeout_ms = req.timeout_ms > 0 ? req.timeout_ms
            : (config_.default_timeout_ms > 0 ? config_.default_timeout_ms : 15000);
        context.policy = policy;
        context.request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        context.network_epoch = network_epoch_.load(std::memory_order_acquire);
        context.request.network_epoch = context.network_epoch;
        context.request.retry_safe = req.retry_safe || is_replayable_safe_method(context.request);
        context.max_retries = context.request.retry_safe ? 1 : 0;
        context.deadline_ms = started_ms + context.request.timeout_ms;

        ret = orchestrator_->execute(context, resp,
            [this](orchestrator::RequestContext &request_context, engine::Response &response) {
                return execute_transport(request_context, response);
            });
        if (resp.total_ms < 0) resp.total_ms = monotonic_ms() - started_ms;
        if (resp.failure_stage.empty() && ret != ASTERNET_OK) {
            resp.failure_stage = failure_stage_for(ret);
        }
        resp.attempts = std::max(resp.attempts, context.attempts);
        if (out_actual_proto) *out_actual_proto = resp.protocol;
        if (out_degraded) *out_degraded = resp.degraded;
        metrics_collector = metrics_collector_;
        report_metrics(std::move(metrics_collector), context.request_id, context.network_epoch,
                       context, resp, ret);
    }
    return ret;
}

int Client::execute_transport(orchestrator::RequestContext &context, engine::Response &response) {
    if (!dns_resolver_ || !connection_pool_ || !quality_prober_ || !selector_) {
        response.err_code = ASTERNET_ERR_INTERNAL;
        response.failure_stage = "initialization";
        return response.err_code;
    }

    const dns::ResolveResult resolution = dns_resolver_->resolve_with_metadata(
        context.request.host, context.network_epoch, context.request.timeout_ms);
    if (context.attempts <= 1) {
        context.dns_cache_hit = resolution.cache_hit;
    }
    response.dns_ms = resolution.elapsed_ms;
    if (resolution.error != ASTERNET_OK || resolution.addresses.empty()) {
        response.err_code = ASTERNET_ERR_DNS;
        response.failure_stage = "dns";
        quality_prober_->observe(false, -1);
        return response.err_code;
    }

    const size_t max_addresses = context.request.retry_safe
        ? std::min<size_t>(2, resolution.addresses.size()) : 1;
    int result = ASTERNET_ERR_CONNECT;
    for (size_t index = 0; index < max_addresses; ++index) {
        engine::Request attempt = context.request;
        attempt.connect_host = resolution.addresses[index].ip;
        attempt.dns_ms = resolution.elapsed_ms;

        connection::Origin origin;
        origin.host = attempt.host;
        origin.port = attempt.port;
        origin.network_epoch = context.network_epoch;
        const connection::ConnectionLease lease = connection_pool_->acquire(origin);
        result = selector_->request_with_policy(attempt, context.policy, response, nullptr, nullptr);
        connection_pool_->release(lease, result == ASTERNET_OK);

        const int observed_rtt = response.ttfb_ms >= 0
            ? static_cast<int>(response.ttfb_ms) : static_cast<int>(response.total_ms);
        dns_resolver_->report_connection_result(attempt.host, attempt.connect_host,
                                                context.network_epoch, result == ASTERNET_OK,
                                                observed_rtt);
        quality_prober_->observe(result == ASTERNET_OK, observed_rtt);
        if (result == ASTERNET_OK || result != ASTERNET_ERR_CONNECT) break;
    }
    if (response.dns_ms < 0) response.dns_ms = resolution.elapsed_ms;
    if (result != ASTERNET_OK && response.failure_stage.empty()) {
        response.failure_stage = failure_stage_for(result);
    }
    return result;
}

void Client::report_metrics(std::shared_ptr<monitor::MetricsCollector> collector,
                            uint64_t request_id, uint64_t network_epoch,
                            const orchestrator::RequestContext &context,
                            const engine::Response &response, int result) {
    asternet_response_info_t info{};
    info.result = static_cast<asternet_result_t>(result);
    info.http_status = response.http_status;
    info.protocol = response.protocol;
    info.degraded = response.degraded ? 1 : 0;
    info.body_size = response.body.size();
    info.body_copied = response.body.size();
    info.dns_ms = response.dns_ms;
    info.connect_ms = response.connect_ms;
    info.tls_ms = response.tls_ms;
    info.ttfb_ms = response.ttfb_ms;
    info.total_ms = response.total_ms;
    monitor::RequestMetrics metrics;
    metrics.response = info;
    metrics.request_id = request_id;
    metrics.network_epoch = network_epoch;
    metrics.attempts = response.attempts;
    metrics.connection_reused = response.connection_reused;
    metrics.cache_hit = context.dns_cache_hit;
    metrics.deduplicated = context.deduplicated;
    metrics.failure_stage = response.failure_stage;
    enqueue_metrics(std::move(metrics), std::move(collector));
}

void Client::enqueue_metrics(monitor::RequestMetrics metrics,
                             std::shared_ptr<monitor::MetricsCollector> collector) {
    {
        std::lock_guard<std::mutex> lock(metrics_queue_mutex_);
        constexpr size_t kMaxQueuedMetrics = 256;
        if (metrics_queue_.size() >= kMaxQueuedMetrics) {
            metrics_queue_.pop_front();
        }
        metrics_queue_.push_back(PendingMetrics{std::move(metrics), std::move(collector)});
    }
    metrics_queue_cv_.notify_one();
}

void Client::metrics_worker_loop() {
    for (;;) {
        PendingMetrics pending;
        {
            std::unique_lock<std::mutex> lock(metrics_queue_mutex_);
            metrics_queue_cv_.wait(lock, [this] {
                return metrics_worker_stop_ || !metrics_queue_.empty();
            });
            if (metrics_queue_.empty()) {
                if (metrics_worker_stop_) return;
                continue;
            }
            pending = std::move(metrics_queue_.front());
            metrics_queue_.pop_front();
        }
        try {
            if (metrics_shadow_collector_) {
                metrics_shadow_collector_->report_request(pending.metrics);
            }
            if (pending.collector && pending.collector != metrics_shadow_collector_) {
                pending.collector->report_request(pending.metrics);
            }
        } catch (...) {
            ASTER_LOG_WARN("asternet-client", "metrics report callback failed");
        }
    }
}

const char *Client::failure_stage_for(int result) {
    switch (result) {
    case ASTERNET_ERR_DNS: return "dns";
    case ASTERNET_ERR_CONNECT: return "connect";
    case ASTERNET_ERR_TLS: return "tls";
    case ASTERNET_ERR_TIMEOUT: return "timeout";
    case ASTERNET_ERR_PROTOCOL: return "protocol";
    case ASTERNET_ERR_CANCELED: return "canceled";
    default: return "internal";
    }
}

}  // namespace asternet

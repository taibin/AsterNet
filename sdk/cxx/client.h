/*
 * AsterNet 网络核心 —— C++ 内部门面
 *
 * Client 是 C++ 侧的顶层对象，被 C ABI 层（abi/abi.cpp）包装为 asternet_client_t 句柄。
 * 后续各模块（engine / connection / dns / sdt / orchestrator / monitor）由 Client 持有，
 * 阶段 1 起逐步注入。
 *
 * 配置中的协议偏好指针仅在构造期间读取；运行时状态由 Client 自己持有。
 */
#ifndef ASTERNET_CLIENT_H
#define ASTERNET_CLIENT_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <deque>

#include "asternet/asternet.h"
#include "connection/connection_pool.h"
#include "dns/dns_resolver.h"
#include "engine/engine.h"
#include "engine/http1_engine.h"
#include "engine/http2_engine.h"
#include "engine/protocol_selector.h"
#include "monitor/metrics.h"
#include "orchestrator/interceptor.h"
#include "protocol/protocol.h"
#include "sdt/quality_prober.h"

#ifdef ASTERNET_ENABLE_XQUIC
#include "engine/quic_engine.h"
#endif

namespace asternet {

class Client {
public:
    explicit Client(const asternet_client_config_t &cfg);
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    static bool check_abi(uint32_t abi_version);

    const asternet_client_config_t &config() const { return config_; }
    const std::string &ca_cert_pem() const { return ca_cert_pem_; }

    void on_network_change(asternet_network_t net);
    std::string dump_diag() const;
    int prefetch(const std::string &host);
    void set_quality_change_callback(asternet_quality_callback_t callback, void *user_data);
    int set_metrics_collector(std::shared_ptr<monitor::MetricsCollector> collector);
    std::string trace_route(const std::string &host, uint16_t port);

    // 统一请求入口：经 ProtocolSelector 降级链选择引擎（H3→H2→H1.1）。
    int request(const engine::Request &req, engine::Response &resp);

    int request_with_policy(const engine::Request &req, asternet_protocol_policy_t policy,
                            engine::Response &resp, asternet_protocol_t *out_actual_proto = nullptr,
                            bool *out_degraded = nullptr);

private:
    struct QualityCallbackState {
        asternet_quality_callback_t callback = nullptr;
        void *user_data = nullptr;
    };

    int execute_transport(orchestrator::RequestContext &context, engine::Response &response);
    void report_metrics(std::shared_ptr<monitor::MetricsCollector> collector,
                        uint64_t request_id, uint64_t network_epoch,
                        const orchestrator::RequestContext &context,
                        const engine::Response &response, int result);
    void publish_quality_change(const sdt::QualitySnapshot &snapshot);
    void enqueue_metrics(monitor::RequestMetrics metrics,
                         std::shared_ptr<monitor::MetricsCollector> collector);
    void metrics_worker_loop();
    static const char *failure_stage_for(int result);

    struct PendingMetrics {
        monitor::RequestMetrics metrics;
        std::shared_ptr<monitor::MetricsCollector> collector;
    };

    asternet_client_config_t config_{};
    std::string ca_cert_pem_;
    std::atomic<bool> destroyed_{false};
    std::atomic<uint64_t> next_request_id_{1};
    std::atomic<uint64_t> network_epoch_{0};
    mutable std::shared_mutex lifecycle_mutex_;

    std::shared_ptr<engine::NetworkEngine> h1_engine_;
    std::shared_ptr<engine::NetworkEngine> h2_engine_;
    std::shared_ptr<engine::NetworkEngine> h3_engine_;
    std::unique_ptr<engine::ProtocolSelector> selector_;
    std::shared_ptr<dns::SmartDnsResolver> dns_resolver_;
    std::shared_ptr<connection::ConnectionPool> connection_pool_;
    std::shared_ptr<sdt::QualityProber> quality_prober_;
    std::shared_ptr<QualityCallbackState> quality_callback_state_;
    sdt::QualitySnapshot last_quality_snapshot_;
    std::unique_ptr<orchestrator::RequestOrchestrator> orchestrator_;
    std::shared_ptr<monitor::MetricsCollector> metrics_collector_;
    std::shared_ptr<monitor::MetricsCollector> metrics_shadow_collector_;
    std::mutex metrics_queue_mutex_;
    std::condition_variable metrics_queue_cv_;
    std::deque<PendingMetrics> metrics_queue_;
    bool metrics_worker_stop_ = false;
    std::thread metrics_worker_thread_;
    std::unique_ptr<protocol::GatewayProtocol> gateway_protocol_;
};

}  // namespace asternet

#endif  // ASTERNET_CLIENT_H

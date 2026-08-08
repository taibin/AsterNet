/*
 * AsterNet 网络核心 —— C++ 内部门面实现（阶段 0 最小骨架）
 */
#include "client.h"

#include <cstring>

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

}  // namespace

bool Client::check_abi(uint32_t abi_version) {
    const uint32_t expected_major = ASTERNET_ABI_VERSION >> 16;
    const uint32_t got_major = abi_version >> 16;
    return got_major == expected_major;
}

Client::Client(const asternet_client_config_t &cfg)
    : config_(cfg), ca_cert_pem_(cfg.ca_cert_pem != nullptr ? cfg.ca_cert_pem : "") {
    // 构造三引擎 + 协议选择器（降级链 H3→H2→H1.1）
    const bool allow_insecure = config_.allow_insecure_tls_for_testing != 0;
    h1_engine_ = std::make_shared<engine::Http1Engine>(allow_insecure, ca_cert_pem_);
    h2_engine_ = std::make_shared<engine::Http2Engine>(allow_insecure, ca_cert_pem_);
#ifdef ASTERNET_ENABLE_XQUIC
    if (config_.enable_http3 != 0) {
        h3_engine_ = std::make_shared<engine::QuicEngine>(allow_insecure, ca_cert_pem_);
    }
#endif
    selector_ = std::make_unique<engine::ProtocolSelector>(h3_engine_, h2_engine_, h1_engine_);
}

Client::~Client() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    destroyed_.store(true, std::memory_order_release);
    selector_.reset();
    h3_engine_.reset();
    h2_engine_.reset();
    h1_engine_.reset();
}

void Client::on_network_change(asternet_network_t /*net*/) {
    if (is_active_client(this)) return;
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    // TODO(阶段3): 通知 connection::ConnectionMigration 执行 QUIC 连接迁移；
    //              通知 sdt::QualityProber 重置探测；通知 dns 预解析切换后域名。
}

std::string Client::dump_diag() const {
    if (is_active_client(const_cast<Client *>(this))) return "{}";
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    // TODO(阶段1): 输出线程状态 / 连接池 / DNS 缓存快照。
    return std::string("{}");
}

int Client::request(const engine::Request &req, engine::Response &resp) {
    if (is_active_client(this) || g_active_client_count >= kMaxActiveClients) {
        return ASTERNET_ERR_INTERNAL;
    }
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    ActiveRequestGuard active_guard(this);
    if (destroyed_.load(std::memory_order_acquire)) return ASTERNET_ERR_CANCELED;
    if (!selector_) return ASTERNET_ERR_INTERNAL;
    return selector_->request_with_policy(req, ASTERNET_POLICY_AUTO, resp, nullptr, nullptr);
}

int Client::request_with_policy(const engine::Request &req, asternet_protocol_policy_t policy,
                                engine::Response &resp, asternet_protocol_t *out_actual_proto,
                                bool *out_degraded) {
    if (is_active_client(this) || g_active_client_count >= kMaxActiveClients) {
        return ASTERNET_ERR_INTERNAL;
    }
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    ActiveRequestGuard active_guard(this);
    ASTER_LOG_INFO("asternet-client", "==> %s:%d %s %s policy=%d",
               req.host.c_str(), req.port, req.method.c_str(), req.path.c_str(), static_cast<int>(policy));
    if (destroyed_.load(std::memory_order_acquire)) return ASTERNET_ERR_CANCELED;
    if (!selector_) return ASTERNET_ERR_INTERNAL;
    int ret = selector_->request_with_policy(req, policy, resp, out_actual_proto, out_degraded);
    int actual = out_actual_proto ? (int)*out_actual_proto : -1;
    ASTER_LOG_INFO("asternet-client", "<== ret=%d actual_proto=%d status=%d total_ms=%lld body_len=%zu",
               ret, actual, resp.http_status, (long long)resp.total_ms, resp.body.size());
    return ret;
}

}  // namespace asternet

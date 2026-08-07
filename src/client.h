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
#include <memory>
#include <mutex>
#include <string>

#include "asternet/asternet.h"
#include "engine/engine.h"
#include "engine/http1_engine.h"
#include "engine/http2_engine.h"
#include "engine/protocol_selector.h"

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

    // 统一请求入口：经 ProtocolSelector 降级链选择引擎（H3→H2→H1.1）。
    int request(const engine::Request &req, engine::Response &resp);

    int request_with_policy(const engine::Request &req, asternet_protocol_policy_t policy,
                            engine::Response &resp, asternet_protocol_t *out_actual_proto = nullptr,
                            bool *out_degraded = nullptr);

private:
    asternet_client_config_t config_{};
    std::string ca_cert_pem_;
    std::atomic<bool> destroyed_{false};
    mutable std::mutex lifecycle_mutex_;

    std::shared_ptr<engine::NetworkEngine> h1_engine_;
    std::shared_ptr<engine::NetworkEngine> h2_engine_;
    std::shared_ptr<engine::NetworkEngine> h3_engine_;
    std::unique_ptr<engine::ProtocolSelector> selector_;
};

}  // namespace asternet

#endif  // ASTERNET_CLIENT_H

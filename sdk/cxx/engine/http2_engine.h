/*
 * AsterNet 网络核心 —— HTTP/2 引擎（集成 nghttp2）
 *
 * 基于 nghttp2 + boringssl TLS（ALPN 协商 h2）实现 HTTP/2 请求：
 *   TCP connect → TLS 握手(ALPN h2) → nghttp2 session → submit_request → 读写循环 → 响应
 *
 * 阶段 1：单请求同步，不复用连接。证书校验 POC 跳过。
 *
 * 依赖：libnghttp2.a + libssl.a + libcrypto.a
 */
#ifndef ASTERNET_HTTP2_ENGINE_H
#define ASTERNET_HTTP2_ENGINE_H

#include "engine.h"

#include <memory>
#include <mutex>
#include <utility>

namespace asternet {
namespace engine {

class Http2Engine : public NetworkEngine {
public:
    explicit Http2Engine(bool allow_insecure_tls_for_testing, std::string ca_cert_pem = {});
    ~Http2Engine() override;

    EngineType type() const override { return EngineType::kHttp2; }
    EngineCaps caps() const override {
        EngineCaps c{};
        c.http2 = true;
        return c;
    }

    int request(const Request &req, Response &resp) override;
    int migrate_connection() override;

private:
    struct PooledConnection;

    int ensure_connection(const Request &req, Response &resp, int64_t deadline_ms, bool &reused);
    void close_connection();

    bool allow_insecure_tls_for_testing_ = false;
    std::string ca_cert_pem_;
    std::mutex mutex_;
    std::unique_ptr<PooledConnection> connection_;
};

}  // namespace engine
}  // namespace asternet

#endif  // ASTERNET_HTTP2_ENGINE_H

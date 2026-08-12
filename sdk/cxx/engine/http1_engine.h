/*
 * AsterNet 网络核心 —— HTTP/1.1 引擎（自研）
 *
 * 基于 boringssl TLS 实现 HTTP/1.1 请求：
 *   socket → TLS 握手 → 发请求行+头+body → 读响应（状态行+头+body）
 *   支持 Content-Length 与 Transfer-Encoding: chunked 两种 body 定界。
 *
 * 复用单个同 origin 的 Keep-Alive TLS 连接。HTTP/1.1 不支持同连接并发请求，因此
 * 请求在引擎内串行；网络切换或读写异常会立即淘汰现有连接。
 *
 * 依赖：boringssl 静态库（libssl + libcrypto）。
 */
#ifndef ASTERNET_HTTP1_ENGINE_H
#define ASTERNET_HTTP1_ENGINE_H

#include "engine.h"

#include <memory>
#include <mutex>
#include <utility>

namespace asternet {
namespace engine {

class Http1Engine : public NetworkEngine {
public:
    explicit Http1Engine(bool allow_insecure_tls_for_testing, std::string ca_cert_pem = {});
    ~Http1Engine() override;

    EngineType type() const override { return EngineType::kHttp1; }
    EngineCaps caps() const override {
        EngineCaps c{};
        c.http1_1 = true;
        return c;
    }

    int request(const Request &req, Response &resp) override;
    int prefetch(const std::string &host) override;
    int migrate_connection() override;

private:
    struct PooledConnection;

    int ensure_connection(const Request &req, Response &resp, bool &reused);
    void close_connection();

    bool allow_insecure_tls_for_testing_ = false;
    std::string ca_cert_pem_;
    std::mutex mutex_;
    std::unique_ptr<PooledConnection> connection_;
};

}  // namespace engine
}  // namespace asternet

#endif  // ASTERNET_HTTP1_ENGINE_H

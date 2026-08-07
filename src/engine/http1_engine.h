/*
 * AsterNet 网络核心 —— HTTP/1.1 引擎（自研）
 *
 * 基于 boringssl TLS 实现 HTTP/1.1 请求：
 *   socket → TLS 握手 → 发请求行+头+body → 读响应（状态行+头+body）
 *   支持 Content-Length 与 Transfer-Encoding: chunked 两种 body 定界。
 *
 * 阶段 1：单请求无连接池（每次新建连接），验证 H1 链路。
 * 后续：Keep-Alive 连接池复用。
 *
 * 依赖：boringssl 静态库（libssl + libcrypto）。
 */
#ifndef ASTERNET_HTTP1_ENGINE_H
#define ASTERNET_HTTP1_ENGINE_H

#include "engine.h"

#include <utility>

namespace asternet {
namespace engine {

class Http1Engine : public NetworkEngine {
public:
    explicit Http1Engine(bool allow_insecure_tls_for_testing, std::string ca_cert_pem = {})
        : allow_insecure_tls_for_testing_(allow_insecure_tls_for_testing),
          ca_cert_pem_(std::move(ca_cert_pem)) {}
    ~Http1Engine() override = default;

    EngineType type() const override { return EngineType::kHttp1; }
    EngineCaps caps() const override {
        EngineCaps c{};
        c.http1_1 = true;
        return c;
    }

    int request(const Request &req, Response &resp) override;

private:
    bool allow_insecure_tls_for_testing_ = false;
    std::string ca_cert_pem_;
};

}  // namespace engine
}  // namespace asternet

#endif  // ASTERNET_HTTP1_ENGINE_H

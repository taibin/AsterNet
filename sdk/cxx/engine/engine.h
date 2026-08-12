/*
 * AsterNet 网络核心 —— 传输引擎接口
 *
 * NetworkEngine 屏蔽底层协议栈差异（HTTP/1.1 / HTTP/2 / HTTP/3），由 ProtocolSelector
 * 按质量数据与降级策略选择实现。各引擎实现统一的 request() 同步方法。
 *
 * 引擎实现：
 *   - Http1Engine  : HTTP/1.1（自研，boringssl TLS）
 *   - Http2Engine  : HTTP/2  （集成 nghttp2）
 *   - QuicEngine   : HTTP/3  （集成 xquic）
 *
 * 接口为纯虚，不依赖任何第三方类型。
 */
#ifndef ASTERNET_ENGINE_H
#define ASTERNET_ENGINE_H

#include <cstdint>
#include <string>
#include <vector>

#include "asternet/asternet.h"  // asternet_protocol_t / asternet_result_t

namespace asternet {
namespace engine {

// 引擎类型
enum class EngineType {
    kHttp1,    // HTTP/1.1（自研）
    kHttp2,    // HTTP/2  （nghttp2）
    kHttp3,    // HTTP/3  （xquic）
    kCronet,   // 对照基线（未实现）
};

// 引擎能力描述
struct EngineCaps {
    bool http1_1 : 1;
    bool http2   : 1;
    bool http3   : 1;
    bool zero_rtt        : 1;  // 0-RTT
    bool migration       : 1;  // 连接迁移
    bool datagram        : 1;  // QUIC Datagram
};

// 请求头
struct Header {
    std::string name;
    std::string value;
};

// 统一请求
struct Request {
    std::string host;
    // Optional numeric endpoint selected by the resolver. The logical host remains authoritative
    // for HTTP Host/:authority, TLS SNI and certificate hostname verification.
    std::string connect_host;
    uint16_t    port = 443;
    std::string method = "GET";   // GET/POST/PUT/DELETE...
    std::string path = "/";       // 含 query
    std::string body;             // POST/PUT body，GET 传空
    std::vector<Header> headers;  // 自定义头（不含 Host/由引擎注入）
    int         timeout_ms = 15000;
    bool        idempotent = false;
    // Whether the request may be replayed by retry, address failover or protocol fallback.
    // This is stricter than HTTP method semantics: writes require a verified idempotency key.
    bool        retry_safe = false;
    bool        allow_insecure_tls_for_testing = false;
    std::string ca_cert_pem;
    size_t      max_response_body_bytes = 16 * 1024 * 1024;
    int64_t     dns_ms = -1;
    uint64_t    network_epoch = 0;
};

// 统一响应
struct Response {
    int         err_code = 0;     // asternet_result_t，ASTERNET_OK 表示成功
    int         http_status = 0;  // HTTP 状态码
    asternet_protocol_t protocol = ASTERNET_PROTOCOL_UNKNOWN;
    bool        degraded = false;
    std::string body;
    // 逐阶段耗时（毫秒），-1 未采集
    int64_t dns_ms = -1;
    int64_t connect_ms = -1;
    int64_t tls_ms = -1;
    int64_t ttfb_ms = -1;
    int64_t total_ms = -1;
    bool connection_reused = false;
    int attempts = 1;
    std::string failure_stage;
};

// 传输引擎抽象。各引擎实现 request()，ProtocolSelector 按策略选择引擎调用。
class NetworkEngine {
public:
    virtual ~NetworkEngine() = default;

    virtual EngineType type() const = 0;
    virtual EngineCaps caps() const = 0;

    // 同步请求：阻塞至完成或超时。返回 ASTERNET_OK 或 asternet_result_t 错误码。
    virtual int request(const Request &req, Response &resp) = 0;

    // 未提供持久连接实现的引擎必须明确返回 UNSUPPORTED，不能伪报预连接成功。
    virtual int prefetch(const std::string &host) {
        (void)host;
        return ASTERNET_ERR_UNSUPPORTED;
    }

    // 同上，只有实际保存连接状态的 QUIC 实现才可迁移。
    virtual int migrate_connection() { return ASTERNET_ERR_UNSUPPORTED; }
};

}  // namespace engine
}  // namespace asternet

#endif  // ASTERNET_ENGINE_H

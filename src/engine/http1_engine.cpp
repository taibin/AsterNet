/*
 * AsterNet 网络核心 —— HTTP/1.1 引擎实现（自研，boringssl TLS）
 *
 * 流程：DNS → TCP socket → TLS 握手 → 发请求 → 读响应（Content-Length / chunked）。
 * POC：证书校验跳过（生产必须校验），无 Keep-Alive 连接池（每次新建连接）。
 */
#include "engine/http1_engine.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include <cstdio>
#include <cstdlib>
#include <cerrno>

#include "platform/log.h"
#include "platform/tls.h"
#include <algorithm>
#include <cstring>
#include <climits>
#include <string>
#include <vector>

namespace asternet {
namespace engine {

namespace {

// 阻塞 connect 带超时（SO_RCVTIMEO/SO_SNDTIMEO）
int connect_with_timeout(const char *host, uint16_t port, int timeout_ms) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;  // 同时支持 IPv4/IPv6，兼容纯 IPv6+NAT64 网络
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == nullptr) return -1;

    int fd = -1;
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

// 读取至缓冲区填满或返回（同步，依赖 socket 超时）
ssize_t read_n(SSL *ssl, int fd, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = SSL_read(ssl, buf + got, n - got);
        if (r > 0) { got += r; continue; }
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
        break;
    }
    return got;
}

enum class ChunkParseResult { kIncomplete, kInvalid, kComplete };

// 按 chunk 边界解析 body，避免响应数据中的相同字节串被误当作结束标记。
ChunkParseResult decode_chunked(const std::string &raw, std::string &out) {
    out.clear();
    size_t pos = 0;
    for (;;) {
        const size_t line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos) return ChunkParseResult::kIncomplete;
        const std::string size_line = raw.substr(pos, line_end - pos);
        errno = 0;
        char *end = nullptr;
        const unsigned long long chunk_size = std::strtoull(size_line.c_str(), &end, 16);
        if (end == size_line.c_str() || errno == ERANGE
            || (*end != '\0' && *end != ';')) {
            return ChunkParseResult::kInvalid;
        }
        pos = line_end + 2;
        if (chunk_size == 0) {
            // 零 chunk 后是可选 trailer，空行表示消息结束。
            for (;;) {
                const size_t trailer_end = raw.find("\r\n", pos);
                if (trailer_end == std::string::npos) {
                    return ChunkParseResult::kIncomplete;
                }
                if (trailer_end == pos) return ChunkParseResult::kComplete;
                pos = trailer_end + 2;
            }
        }
        if (chunk_size > raw.size() - pos) return ChunkParseResult::kIncomplete;
        const size_t body_end = pos + static_cast<size_t>(chunk_size);
        if (raw.size() < body_end + 2) return ChunkParseResult::kIncomplete;
        if (raw.compare(body_end, 2, "\r\n") != 0) return ChunkParseResult::kInvalid;
        out.append(raw, pos, static_cast<size_t>(chunk_size));
        pos = body_end + 2;
    }
}

}  // namespace

int Http1Engine::request(const Request &req, Response &resp) {
    resp.protocol = ASTERNET_PROTOCOL_HTTP_1_1;
    ASTER_LOG_INFO("asternet-h1", "==> %s:%d %s %s timeout_ms=%d",
               req.host.c_str(), req.port, req.method.c_str(), req.path.c_str(), req.timeout_ms);
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    // 1. TCP connect
    int fd = connect_with_timeout(req.host.c_str(), req.port, req.timeout_ms);
    if (fd < 0) {
        resp.err_code = ASTERNET_ERR_CONNECT;
        return ASTERNET_ERR_CONNECT;
    }

    // 2. TLS 握手
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) { close(fd); resp.err_code = ASTERNET_ERR_TLS; return ASTERNET_ERR_TLS; }
    const bool allow_insecure = allow_insecure_tls_for_testing_
                             || req.allow_insecure_tls_for_testing;
    SSL_CTX_set_verify(ctx, allow_insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, nullptr);
    const std::string &ca_bundle = req.ca_cert_pem.empty() ? ca_cert_pem_ : req.ca_cert_pem;
    const bool trust_ready = allow_insecure
        || (!ca_bundle.empty() ? asternet::platform::load_ca_bundle(ctx, ca_bundle)
                               : SSL_CTX_set_default_verify_paths(ctx) == 1);
    if (!trust_ready) {
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_TLS;
        return ASTERNET_ERR_TLS;
    }

    SSL *ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_OUT_OF_MEMORY;
        return ASTERNET_ERR_OUT_OF_MEMORY;
    }
    SSL_set_tlsext_host_name(ssl, req.host.c_str());  // SNI
    if (!allow_insecure && SSL_set1_host(ssl, req.host.c_str()) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_TLS;
        return ASTERNET_ERR_TLS;
    }
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
        resp.err_code = ASTERNET_ERR_TLS;
        return ASTERNET_ERR_TLS;
    }

    // 3. 构造请求
    // Host 头：非默认端口需显式携带端口号（RFC 7230）
    std::string host_hdr = req.host;
    if (req.port != 443 && req.port != 80) {
        host_hdr += ":" + std::to_string(req.port);
    }
    std::string req_str = req.method + " " + req.path + " HTTP/1.1\r\n";
    req_str += "Host: " + host_hdr + "\r\n";
    req_str += "User-Agent: asternet/0.1\r\n";
    req_str += "Connection: close\r\n";  // POC 不复用连接
    bool has_body = !req.body.empty();
    if (has_body) {
        req_str += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    }
    for (const auto &h : req.headers) {
        req_str += h.name + ": " + h.value + "\r\n";
    }
    req_str += "\r\n";
    if (has_body) req_str += req.body;

    size_t written = 0;
    while (written < req_str.size()) {
        const size_t remaining = req_str.size() - written;
        const int write_size = static_cast<int>(std::min(remaining, static_cast<size_t>(INT_MAX)));
        const int n = SSL_write(ssl, req_str.data() + written, write_size);
        if (n <= 0) {
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            resp.err_code = ASTERNET_ERR_PROTOCOL;
            return ASTERNET_ERR_PROTOCOL;
        }
        written += static_cast<size_t>(n);
    }

    // 4. 读响应（先读 header 段，再按 Content-Length / chunked 读 body）
    std::string raw;
    char buf[4096];
    size_t header_end = std::string::npos;
    constexpr size_t kMaxResponseHeaders = 64 * 1024;
    while (header_end == std::string::npos && raw.size() <= kMaxResponseHeaders) {
        ssize_t r = SSL_read(ssl, buf, sizeof(buf));
        if (r <= 0) break;
        raw.append(buf, r);
        header_end = raw.find("\r\n\r\n");
    }
    if (header_end == std::string::npos || raw.size() > kMaxResponseHeaders) {
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
        resp.err_code = ASTERNET_ERR_PROTOCOL;
        return ASTERNET_ERR_PROTOCOL;
    }

    // 解析状态行
    size_t sp1 = raw.find(' ');
    if (sp1 != std::string::npos) {
        size_t sp2 = raw.find(' ', sp1 + 1);
        if (sp2 != std::string::npos) {
            resp.http_status = atoi(raw.c_str() + sp1 + 1);
        }
    }

    // 判断 body 定界
    std::string headers = raw.substr(0, header_end);
    std::string lower;
    lower.reserve(headers.size());
    for (char c : headers) lower += (char)tolower(c);

    bool chunked = lower.find("transfer-encoding: chunked") != std::string::npos;
    size_t cl = std::string::npos;
    {
        size_t p = lower.find("content-length:");
        if (p != std::string::npos) {
            cl = atoi(headers.c_str() + p + strlen("content-length:"));
        }
    }

    std::string body = raw.substr(header_end + 4);
    const size_t max_body = req.max_response_body_bytes;
    if (chunked) {
        std::string decoded_body;
        ChunkParseResult parse_result = ChunkParseResult::kIncomplete;
        while (parse_result == ChunkParseResult::kIncomplete
               && body.size() <= max_body + kMaxResponseHeaders) {
            parse_result = decode_chunked(body, decoded_body);
            if (parse_result != ChunkParseResult::kIncomplete) break;
            ssize_t r = SSL_read(ssl, buf, sizeof(buf));
            if (r <= 0) break;
            body.append(buf, r);
        }
        if (parse_result != ChunkParseResult::kComplete) {
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            resp.err_code = body.size() > max_body + kMaxResponseHeaders
                                ? ASTERNET_ERR_BUFFER_TOO_SMALL
                                                    : ASTERNET_ERR_PROTOCOL;
            return resp.err_code;
        }
        if (decoded_body.size() > max_body) {
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
            return ASTERNET_ERR_BUFFER_TOO_SMALL;
        }
        resp.body = std::move(decoded_body);
    } else if (cl != std::string::npos) {
        if (cl > max_body) {
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
            return ASTERNET_ERR_BUFFER_TOO_SMALL;
        }
        while (body.size() < cl) {
            const size_t remaining = cl - body.size();
            const size_t read_size = std::min(remaining, sizeof(buf));
            ssize_t r = SSL_read(ssl, buf, read_size);
            if (r <= 0) break;
            body.append(buf, r);
        }
        if (body.size() < cl) {
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            resp.err_code = ASTERNET_ERR_PROTOCOL;
            return ASTERNET_ERR_PROTOCOL;
        }
        resp.body = body.substr(0, cl);
    } else {
        // 无 Content-Length 且非 chunked（Connection: close），读到 EOF
        while (body.size() <= max_body) {
            const size_t remaining = max_body - body.size();
            const size_t read_size = std::min(remaining + 1, sizeof(buf));
            ssize_t r = SSL_read(ssl, buf, read_size);
            if (r <= 0) break;
            body.append(buf, r);
        }
        if (body.size() > max_body) {
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
            return ASTERNET_ERR_BUFFER_TOO_SMALL;
        }
        resp.body = body;
    }
    if (resp.body.size() > req.max_response_body_bytes) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
        return ASTERNET_ERR_BUFFER_TOO_SMALL;
    }

    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    resp.total_ms = (t_end.tv_sec - t_start.tv_sec) * 1000
                    + (t_end.tv_nsec - t_start.tv_nsec) / 1000000;

    ASTER_LOG_INFO("asternet-h1", "<== OK status=%d body_len=%zu total_ms=%lld",
               resp.http_status, resp.body.size(), (long long)resp.total_ms);
    resp.err_code = ASTERNET_OK;
    return ASTERNET_OK;
}

}  // namespace engine
}  // namespace asternet

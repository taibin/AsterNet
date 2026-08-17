/*
 * AsterNet 网络核心 —— HTTP/1.1 引擎实现（自研，boringssl TLS）
 *
 * 流程：DNS → TCP socket → TLS 握手 → 发请求 → 读响应（Content-Length / chunked）。
 * 当前支持同 origin Keep-Alive 复用；网络切换或读写异常会立即淘汰现有连接。
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
#include <chrono>

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
int64_t monotonic_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

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

struct Http1Engine::PooledConnection {
    int fd = -1;
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    std::string host;
    std::string endpoint;
    uint16_t port = 0;
    std::string ca_cert_pem;
    uint64_t network_epoch = 0;
    bool allow_insecure = false;
};

Http1Engine::Http1Engine(bool allow_insecure_tls_for_testing, std::string ca_cert_pem)
    : allow_insecure_tls_for_testing_(allow_insecure_tls_for_testing),
      ca_cert_pem_(std::move(ca_cert_pem)) {}

Http1Engine::~Http1Engine() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_connection();
}

void Http1Engine::close_connection() {
    if (!connection_) return;
    if (connection_->ssl != nullptr) SSL_free(connection_->ssl);
    if (connection_->ctx != nullptr) SSL_CTX_free(connection_->ctx);
    if (connection_->fd >= 0) close(connection_->fd);
    connection_.reset();
}

int Http1Engine::ensure_connection(const Request &req, Response &resp, bool &reused) {
    const std::string endpoint = req.connect_host.empty() ? req.host : req.connect_host;
#ifdef ASTERNET_ALLOW_INSECURE_TLS_FOR_TESTING
    const bool allow_insecure = allow_insecure_tls_for_testing_;
#else
    const bool allow_insecure = false;
#endif
    const std::string ca_bundle = req.ca_cert_pem.empty() ? ca_cert_pem_ : req.ca_cert_pem;
    if (connection_ && connection_->host == req.host && connection_->endpoint == endpoint
        && connection_->port == req.port && connection_->ca_cert_pem == ca_bundle
        && connection_->network_epoch == req.network_epoch
        && connection_->allow_insecure == allow_insecure) {
        reused = true;
        return ASTERNET_OK;
    }
    close_connection();

    const int64_t connect_start = monotonic_ms();
    const int fd = connect_with_timeout(endpoint.c_str(), req.port, req.timeout_ms);
    resp.connect_ms = monotonic_ms() - connect_start;
    if (fd < 0) return ASTERNET_ERR_CONNECT;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        close(fd);
        return ASTERNET_ERR_TLS;
    }
    SSL_CTX_set_verify(ctx, allow_insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, nullptr);
    const bool trust_ready = allow_insecure
        || (!ca_bundle.empty() ? asternet::platform::load_ca_bundle(ctx, ca_bundle)
                               : SSL_CTX_set_default_verify_paths(ctx) == 1);
    if (!trust_ready) {
        SSL_CTX_free(ctx);
        close(fd);
        return ASTERNET_ERR_TLS;
    }
    SSL *ssl = SSL_new(ctx);
    if (ssl == nullptr) {
        SSL_CTX_free(ctx);
        close(fd);
        return ASTERNET_ERR_OUT_OF_MEMORY;
    }
    if (SSL_set_tlsext_host_name(ssl, req.host.c_str()) != 1
        || (!allow_insecure && SSL_set1_host(ssl, req.host.c_str()) != 1)) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return ASTERNET_ERR_TLS;
    }
    SSL_set_fd(ssl, fd);
    const int64_t tls_start = monotonic_ms();
    if (SSL_connect(ssl) != 1) {
        resp.tls_ms = monotonic_ms() - tls_start;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return ASTERNET_ERR_TLS;
    }
    resp.tls_ms = monotonic_ms() - tls_start;
    connection_ = std::make_unique<PooledConnection>();
    connection_->fd = fd;
    connection_->ctx = ctx;
    connection_->ssl = ssl;
    connection_->host = req.host;
    connection_->endpoint = endpoint;
    connection_->port = req.port;
    connection_->ca_cert_pem = ca_bundle;
    connection_->network_epoch = req.network_epoch;
    connection_->allow_insecure = allow_insecure;
    return ASTERNET_OK;
}

int Http1Engine::request(const Request &req, Response &resp) {
    std::lock_guard<std::mutex> lock(mutex_);
    resp = Response{};
    resp.protocol = ASTERNET_PROTOCOL_HTTP_1_1;
    resp.dns_ms = req.dns_ms;
    ASTER_LOG_INFO("asternet-h1", "==> %s:%d %s timeout_ms=%d",
               req.host.c_str(), req.port, req.method.c_str(), req.timeout_ms);
    const int64_t t_start = monotonic_ms();

    bool reused = false;
    const int connection_result = ensure_connection(req, resp, reused);
    if (connection_result != ASTERNET_OK) {
        resp.err_code = connection_result;
        resp.failure_stage = connection_result == ASTERNET_ERR_CONNECT ? "connect" : "tls";
        resp.total_ms = monotonic_ms() - t_start;
        return connection_result;
    }
    resp.connection_reused = reused;
    SSL *ssl = connection_->ssl;

    auto has_crlf = [](const std::string &s) {
        return s.find_first_of("\r\n") != std::string::npos;
    };
    if (has_crlf(req.host) || has_crlf(req.connect_host) || has_crlf(req.method)
        || has_crlf(req.path)) {
        close_connection();
        resp.err_code = ASTERNET_ERR_PROTOCOL;
        resp.failure_stage = "header";
        resp.total_ms = monotonic_ms() - t_start;
        return ASTERNET_ERR_PROTOCOL;
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
    req_str += "Connection: keep-alive\r\n";
    bool has_body = !req.body.empty();
    if (has_body) {
        req_str += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    }
    auto append_custom_header = [&](const Header &h) -> bool {
        const std::string name = h.name;
        const std::string value = h.value;
        if (name.find_first_of("\r\n") != std::string::npos
            || value.find_first_of("\r\n") != std::string::npos) {
            return false;
        }
        if (name.empty()) return false;
        for (unsigned char c : name) {
            const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
                || (c >= 'a' && c <= 'z');
            const bool token = alnum || c == '!' || c == '#' || c == '$' || c == '%'
                || c == '&' || c == '\'' || c == '*' || c == '+' || c == '-'
                || c == '.' || c == '^' || c == '_' || c == '`' || c == '|'
                || c == '~';
            if (!token) return false;
        }
        std::string lower_name;
        lower_name.reserve(name.size());
        for (char c : name) lower_name += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (lower_name == "host" || lower_name == "connection" || lower_name == "content-length"
            || lower_name == "transfer-encoding" || lower_name == "keep-alive"
            || lower_name == "proxy-connection" || lower_name == "upgrade") {
            return true;
        }
        req_str += name + ": " + value + "\r\n";
        return true;
    };
    for (const auto &h : req.headers) {
        if (!append_custom_header(h)) {
            close_connection();
            resp.err_code = ASTERNET_ERR_PROTOCOL;
            resp.failure_stage = "header";
            resp.total_ms = monotonic_ms() - t_start;
            return ASTERNET_ERR_PROTOCOL;
        }
    }
    req_str += "\r\n";
    if (has_body) req_str += req.body;

    size_t written = 0;
    while (written < req_str.size()) {
        const size_t remaining = req_str.size() - written;
        const int write_size = static_cast<int>(std::min(remaining, static_cast<size_t>(INT_MAX)));
        const int n = SSL_write(ssl, req_str.data() + written, write_size);
        if (n <= 0) {
            close_connection();
            resp.err_code = ASTERNET_ERR_PROTOCOL;
            resp.failure_stage = "write";
            resp.total_ms = monotonic_ms() - t_start;
            return ASTERNET_ERR_PROTOCOL;
        }
        written += static_cast<size_t>(n);
    }

    // 4. 读响应（先读 header 段，再按 Content-Length / chunked 读 body）
    std::string raw;
    char buf[4096];
    size_t header_end = std::string::npos;
    constexpr size_t kMaxResponseHeaders = 64 * 1024;
    const int64_t first_byte_start = monotonic_ms();
    while (header_end == std::string::npos && raw.size() <= kMaxResponseHeaders) {
        ssize_t r = SSL_read(ssl, buf, sizeof(buf));
        if (r <= 0) break;
        raw.append(buf, r);
        header_end = raw.find("\r\n\r\n");
    }
    if (header_end == std::string::npos || raw.size() > kMaxResponseHeaders) {
        close_connection();
        resp.err_code = ASTERNET_ERR_PROTOCOL;
        resp.failure_stage = "ttfb";
        resp.total_ms = monotonic_ms() - t_start;
        return ASTERNET_ERR_PROTOCOL;
    }
    resp.ttfb_ms = monotonic_ms() - first_byte_start;

    auto parse_status = [&]() {
        size_t sp1 = raw.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = raw.find(' ', sp1 + 1);
            if (sp2 != std::string::npos) {
                resp.http_status = atoi(raw.c_str() + sp1 + 1);
            }
        }
    };
    parse_status();
    while (resp.http_status / 100 == 1) {
        raw.erase(0, header_end + 4);
        header_end = raw.find("\r\n\r\n");
        while (header_end == std::string::npos && raw.size() <= kMaxResponseHeaders) {
            ssize_t r = SSL_read(ssl, buf, sizeof(buf));
            if (r <= 0) break;
            raw.append(buf, r);
            header_end = raw.find("\r\n\r\n");
        }
        if (header_end == std::string::npos || raw.size() > kMaxResponseHeaders) {
            close_connection();
            resp.err_code = ASTERNET_ERR_PROTOCOL;
            resp.failure_stage = "ttfb";
            resp.total_ms = monotonic_ms() - t_start;
            return ASTERNET_ERR_PROTOCOL;
        }
        parse_status();
    }

    // 判断 body 定界
    std::string headers = raw.substr(0, header_end);
    std::string lower;
    lower.reserve(headers.size());
    for (char c : headers) lower += (char)tolower(c);

    bool chunked = false;
    bool connection_close = false;
    size_t cl = std::string::npos;
    bool invalid_response_headers = false;
    size_t line_start = 0;
    while (line_start < headers.size()) {
        size_t line_end = headers.find("\r\n", line_start);
        if (line_end == std::string::npos) line_end = headers.size();
        const std::string line = headers.substr(line_start, line_end - line_start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            std::string lower_name;
            lower_name.reserve(name.size());
            for (char c : name) lower_name += static_cast<char>(tolower(static_cast<unsigned char>(c)));
            std::string lower_value;
            lower_value.reserve(value.size());
            for (char c : value) lower_value += static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (lower_name == "transfer-encoding" && lower_value.find("chunked") != std::string::npos) {
                chunked = true;
            } else if (lower_name == "connection" && lower_value.find("close") != std::string::npos) {
                connection_close = true;
            } else if (lower_name == "content-length") {
                if (value.empty()) {
                    invalid_response_headers = true;
                } else {
                    char *end = nullptr;
                    errno = 0;
                    unsigned long long parsed = strtoull(value.c_str(), &end, 10);
                    while (end != nullptr && (*end == ' ' || *end == '\t')) ++end;
                    if (errno != 0 || end == nullptr || *end != '\0'
                        || parsed > static_cast<unsigned long long>(SIZE_MAX)) {
                        invalid_response_headers = true;
                    } else if (cl != std::string::npos && cl != static_cast<size_t>(parsed)) {
                        invalid_response_headers = true;
                    } else {
                        cl = static_cast<size_t>(parsed);
                    }
                }
            }
        }
        if (line_end == headers.size()) break;
        line_start = line_end + 2;
    }
    if (invalid_response_headers) {
        close_connection();
        resp.err_code = ASTERNET_ERR_PROTOCOL;
        resp.failure_stage = "header";
        resp.total_ms = monotonic_ms() - t_start;
        return ASTERNET_ERR_PROTOCOL;
    }

    std::string body = raw.substr(header_end + 4);
    const size_t max_body = req.max_response_body_bytes;
    const bool no_body_status = req.method == "HEAD"
        || resp.http_status == 204 || resp.http_status == 304;
    if (no_body_status) {
        if (!body.empty()) {
            close_connection();
            resp.err_code = ASTERNET_ERR_PROTOCOL;
            resp.failure_stage = "body";
            resp.total_ms = monotonic_ms() - t_start;
            return ASTERNET_ERR_PROTOCOL;
        }
        resp.body.clear();
    } else if (chunked) {
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
            close_connection();
            resp.err_code = body.size() > max_body + kMaxResponseHeaders
                                 ? ASTERNET_ERR_BUFFER_TOO_SMALL
                                                     : ASTERNET_ERR_PROTOCOL;
            resp.failure_stage = "body";
            resp.total_ms = monotonic_ms() - t_start;
            return resp.err_code;
        }
        if (decoded_body.size() > max_body) {
            close_connection();
            resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
            resp.failure_stage = "body";
            resp.total_ms = monotonic_ms() - t_start;
            return ASTERNET_ERR_BUFFER_TOO_SMALL;
        }
        resp.body = std::move(decoded_body);
    } else if (cl != std::string::npos) {
        if (cl > max_body) {
            close_connection();
            resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
            resp.failure_stage = "body";
            resp.total_ms = monotonic_ms() - t_start;
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
            close_connection();
            resp.err_code = ASTERNET_ERR_PROTOCOL;
            resp.failure_stage = "body";
            resp.total_ms = monotonic_ms() - t_start;
            return ASTERNET_ERR_PROTOCOL;
        }
        if (body.size() > cl) {
            close_connection();
            resp.err_code = ASTERNET_ERR_PROTOCOL;
            resp.failure_stage = "body";
            resp.total_ms = monotonic_ms() - t_start;
            return ASTERNET_ERR_PROTOCOL;
        }
        resp.body = body.substr(0, cl);
    } else {
        // 无 Content-Length 且非 chunked 时，读到 EOF；这类响应不能安全复用连接。
        while (body.size() <= max_body) {
            const size_t remaining = max_body - body.size();
            const size_t read_size = std::min(remaining + 1, sizeof(buf));
            ssize_t r = SSL_read(ssl, buf, read_size);
            if (r <= 0) break;
            body.append(buf, r);
        }
        if (body.size() > max_body) {
            close_connection();
            resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
            resp.failure_stage = "body";
            resp.total_ms = monotonic_ms() - t_start;
            return ASTERNET_ERR_BUFFER_TOO_SMALL;
        }
        resp.body = body;
    }
    if (resp.body.size() > req.max_response_body_bytes) {
        close_connection();
        resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
        resp.failure_stage = "body";
        resp.total_ms = monotonic_ms() - t_start;
        return ASTERNET_ERR_BUFFER_TOO_SMALL;
    }

    if (connection_close || no_body_status || (!chunked && cl == std::string::npos)) {
        close_connection();
    }

    resp.total_ms = monotonic_ms() - t_start;

    ASTER_LOG_INFO("asternet-h1", "<== OK status=%d body_len=%zu total_ms=%lld reused=%d",
               resp.http_status, resp.body.size(), (long long)resp.total_ms, (int)resp.connection_reused);
    resp.err_code = ASTERNET_OK;
    return ASTERNET_OK;
}

int Http1Engine::prefetch(const std::string & /*host*/) {
    // A hostname alone cannot establish a safe TLS connection because port, CA and SNI policy
    // belong to a concrete request. The Client keeps this operation explicitly unsupported.
    return ASTERNET_ERR_UNSUPPORTED;
}

int Http1Engine::migrate_connection() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_connection();
    return ASTERNET_OK;
}

}  // namespace engine
}  // namespace asternet

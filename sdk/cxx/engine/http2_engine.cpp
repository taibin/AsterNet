/*
 * AsterNet 网络核心 —— HTTP/2 引擎实现（nghttp2 + boringssl ALPN h2）
 *
 * 流程：DNS → TCP → TLS(ALPN h2) → nghttp2 session → submit_request → 读写循环 → 响应。
 * nghttp2 通过 send_callback/write_callback 与 TLS socket 交互，回调收集响应 body。
 */
#include "engine/http2_engine.h"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include <cstdio>
#include <chrono>

#include "platform/log.h"
#include "platform/tls.h"
#include <cstring>
#include <string>
#include <vector>

namespace asternet {
namespace engine {

namespace {

int64_t monotonic_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

int select_with_deadline(int fd, bool want_read, bool want_write, int64_t deadline_ms) {
    const int64_t remaining_ms = deadline_ms - monotonic_ms();
    if (remaining_ms <= 0) return 0;
    fd_set rfds;
    fd_set wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (want_read) FD_SET(fd, &rfds);
    if (want_write) FD_SET(fd, &wfds);
    struct timeval tv{static_cast<time_t>(remaining_ms / 1000),
                      static_cast<suseconds_t>((remaining_ms % 1000) * 1000)};
    return select(fd + 1, want_read ? &rfds : nullptr, want_write ? &wfds : nullptr,
                  nullptr, &tv);
}

// 单请求上下文（传递给 nghttp2 回调）
struct H2Ctx {
    static constexpr size_t kMaxResponseBody = 16 * 1024 * 1024;
    SSL *ssl = nullptr;
    std::string req_body;     // 请求 body（POST）
    std::string authority;    // :authority 必须在 nghttp2_submit_request 调用期间保持有效
    std::string pending_data; // send_callback 中读到的响应数据，交由主循环解析
    size_t req_body_sent = 0;
    std::string resp_body;    // 响应 body 累积
    int http_status = 0;
    bool got_status = false;
    bool stream_closed = false;
    bool got_end_stream = false;
    int32_t stream_id = 0;
    uint32_t stream_error = NGHTTP2_NO_ERROR;
    size_t response_header_bytes = 0;
    size_t max_response_body_bytes = 16 * 1024 * 1024;
    bool body_limit_exceeded = false;
    int64_t deadline_ms = 0;
};

// nghttp2 发送回调：把 nghttp2 编码的数据经 TLS 写出。
// 关键：非阻塞 socket + TLS 1.3 post-handshake 场景下，SSL_write 可能因
// 需要先读 server NewSessionTicket 等消息而返回 WANT_READ。此处内部用
// select + SSL_read 排空 TLS 层后重试，确保数据 100% 发出，不把 WOULDBLOCK
// 传播给 nghttp2（否则 nghttp2_session_send 返回 0，preface/SETTINGS/HEADERS
// 永远发不出去）。
static ssize_t send_callback(nghttp2_session * /*session*/, const uint8_t *data,
                             size_t length, int /*flags*/, void *user_data) {
    auto *ctx = static_cast<H2Ctx *>(user_data);
    SSL *ssl = ctx->ssl;
    int fd = SSL_get_fd(ssl);
    size_t sent = 0;

    ASTER_LOG_DEBUG("asternet-h2", "  send_cb ENTER length=%zu ssl=%p fd=%d",
               length, (void*)ssl, fd);

    while (sent < length) {
        int n = SSL_write(ssl, data + sent, (int)(length - sent));
        if (n > 0) { sent += n; continue; }

        int err = SSL_get_error(ssl, n);
        ASTER_LOG_DEBUG("asternet-h2", "  send_cb SSL_write=%d err=%d sent=%zu/%zu",
                   n, err, sent, length);
        if (err == SSL_ERROR_WANT_WRITE) {
            if (select_with_deadline(fd, false, true, ctx->deadline_ms) <= 0)
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            continue;
        }
        if (err == SSL_ERROR_WANT_READ) {
            // TLS post-handshake：server 在 client 发 preface 前先发了
            // NewSessionTicket 等消息，SSL 必须先读才能继续写。
            if (select_with_deadline(fd, true, false, ctx->deadline_ms) <= 0)
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            for (;;) {
                uint8_t drain[4096];
                int read = SSL_read(ssl, drain, sizeof(drain));
                if (read > 0) {
                    ctx->pending_data.append(reinterpret_cast<const char *>(drain), read);
                    break;
                }
                int read_err = SSL_get_error(ssl, read);
                if (read_err == SSL_ERROR_WANT_READ || read_err == SSL_ERROR_WANT_WRITE) {
                    break;
                }
                return NGHTTP2_ERR_CALLBACK_FAILURE;
            }
            continue;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    ASTER_LOG_DEBUG("asternet-h2", "  send_cb EXIT sent=%zu", sent);
    return (ssize_t)sent;
}

// 请求 body 读取回调（POST）
static ssize_t data_read_callback(nghttp2_session * /*session*/, int32_t /*stream_id*/,
                                   uint8_t *buf, size_t length, uint32_t *data_flags,
                                   nghttp2_data_source *source, void *user_data) {
    auto *ctx = static_cast<H2Ctx *>(user_data);
    (void)source;
    size_t remain = ctx->req_body.size() - ctx->req_body_sent;
    size_t n = remain < length ? remain : length;
    if (n > 0) {
        memcpy(buf, ctx->req_body.data() + ctx->req_body_sent, n);
        ctx->req_body_sent += n;
    }
    if (ctx->req_body_sent >= ctx->req_body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return (ssize_t)n;
}

// 响应头回调：取 :status
static int on_header_callback(nghttp2_session * /*session*/, const nghttp2_frame *frame,
                              const uint8_t *name, size_t namelen,
                              const uint8_t *value, size_t valuelen,
                              uint8_t /*flags*/, void *user_data) {
    auto *ctx = static_cast<H2Ctx *>(user_data);
    if (frame->hd.stream_id != ctx->stream_id) return 0;
    ctx->response_header_bytes += namelen + valuelen;
    if (ctx->response_header_bytes > 64 * 1024) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
        if (namelen == 7 && memcmp(name, ":status", 7) == 0) {
            char sbuf[8];
            size_t cpy = valuelen < 7 ? valuelen : 7;
            memcpy(sbuf, value, cpy);
            sbuf[cpy] = '\0';
            ctx->http_status = atoi(sbuf);
            ctx->got_status = true;
        }
    }
    return 0;
}

// 响应 data chunk 回调：累积 body
static int on_data_chunk_recv_callback(nghttp2_session * /*session*/, uint8_t /*flags*/,
                                       int32_t stream_id, const uint8_t *data,
                                       size_t length, void *user_data) {
    auto *ctx = static_cast<H2Ctx *>(user_data);
    if (stream_id != ctx->stream_id) return 0;
    if (length > ctx->max_response_body_bytes
        || ctx->resp_body.size() > ctx->max_response_body_bytes - length) {
        ctx->body_limit_exceeded = true;
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    ctx->resp_body.append(reinterpret_cast<const char *>(data), length);
    return 0;
}

// stream 关闭回调
static int on_stream_close_callback(nghttp2_session * /*session*/, int32_t stream_id,
                                     uint32_t error_code, void *user_data) {
    auto *ctx = static_cast<H2Ctx *>(user_data);
    if (stream_id != ctx->stream_id) return 0;
    ctx->stream_error = error_code;
    ctx->stream_closed = true;
    return 0;
}

static int on_frame_recv_callback(nghttp2_session * /*session*/, const nghttp2_frame *frame,
                                  void *user_data) {
    auto *ctx = static_cast<H2Ctx *>(user_data);
    if (frame->hd.stream_id != ctx->stream_id) return 0;
    const bool response_body_end = frame->hd.type == NGHTTP2_DATA
        || frame->hd.type == NGHTTP2_HEADERS;
    if (response_body_end && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0) {
        ctx->got_end_stream = true;
        ctx->stream_error = NGHTTP2_NO_ERROR;
        ctx->stream_closed = true;
    }
    return 0;
}

int connect_tcp(const char *host, uint16_t port, int64_t deadline_ms) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;  // 同时支持 IPv4/IPv6，兼容纯 IPv6+NAT64 网络
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == nullptr) return -1;
    int fd = -1;
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (fd >= FD_SETSIZE) {
            close(fd);
            fd = -1;
            continue;
        }
        // 非阻塞 socket：收发包由 select 驱动，避免阻塞空转
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        // 短超时作 fallback（阻塞模式下才生效；非阻塞下不会无限阻塞）
        struct timeval tv{3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int cr = connect(fd, p->ai_addr, p->ai_addrlen);
        if (cr == 0) break;
        if (errno == EINPROGRESS) {
            // 非阻塞 connect：等 socket 可写确认连接完成
            if (select_with_deadline(fd, false, true, deadline_ms) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) break;  // 连接成功
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

}  // namespace

int Http2Engine::request(const Request &req, Response &resp) {
    resp = Response{};
    resp.protocol = ASTERNET_PROTOCOL_HTTP_2;
    resp.dns_ms = req.dns_ms;
    ASTER_LOG_INFO("asternet-h2", "==> %s:%d %s timeout_ms=%d",
               req.host.c_str(), req.port, req.method.c_str(), req.timeout_ms);
    const int64_t t_start = monotonic_ms();
    const int64_t deadline_ms = t_start + req.timeout_ms;

    // 1. TCP connect
    const char *endpoint = req.connect_host.empty() ? req.host.c_str() : req.connect_host.c_str();
    const int64_t connect_start = monotonic_ms();
    int fd = connect_tcp(endpoint, req.port, deadline_ms);
    resp.connect_ms = monotonic_ms() - connect_start;
    if (fd < 0) {
        resp.err_code = ASTERNET_ERR_CONNECT;
        resp.failure_stage = "connect";
        resp.total_ms = monotonic_ms() - t_start;
        return ASTERNET_ERR_CONNECT;
    }

    // 2. TLS + ALPN h2
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close(fd); resp.err_code = ASTERNET_ERR_TLS; return ASTERNET_ERR_TLS; }
#ifdef ASTERNET_ALLOW_INSECURE_TLS_FOR_TESTING
    const bool allow_insecure = allow_insecure_tls_for_testing_;
#else
    const bool allow_insecure = false;
#endif
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
    if (SSL_CTX_set_alpn_protos(ctx, (const unsigned char *)"\x02h2", 3) != 0) {
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
    if (SSL_set_tlsext_host_name(ssl, req.host.c_str()) != 1 || SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_TLS;
        return ASTERNET_ERR_TLS;
    }
    if (!allow_insecure && SSL_set1_host(ssl, req.host.c_str()) != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_TLS;
        return ASTERNET_ERR_TLS;
    }
    // 非阻塞 socket 下 SSL_connect 可能需多次重试
    const int64_t tls_start = monotonic_ms();
    int ssl_ret;
    while ((ssl_ret = SSL_connect(ssl)) != 1) {
        int ssl_err = SSL_get_error(ssl, ssl_ret);
        if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
            int selected = select_with_deadline(fd, ssl_err == SSL_ERROR_WANT_READ,
                                                ssl_err == SSL_ERROR_WANT_WRITE, deadline_ms);
            if (selected <= 0) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                close(fd);
                resp.err_code = selected == 0 ? ASTERNET_ERR_TIMEOUT : ASTERNET_ERR_TLS;
                resp.tls_ms = monotonic_ms() - tls_start;
                resp.failure_stage = resp.err_code == ASTERNET_ERR_TIMEOUT ? "tls" : "tls";
                resp.total_ms = monotonic_ms() - t_start;
                return resp.err_code;
            }
            continue;
        }
        SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
        resp.err_code = ASTERNET_ERR_TLS;
        resp.tls_ms = monotonic_ms() - tls_start;
        resp.failure_stage = "tls";
        resp.total_ms = monotonic_ms() - t_start;
        return ASTERNET_ERR_TLS;
    }
    resp.tls_ms = monotonic_ms() - tls_start;
    // 校验 ALPN 协商结果
    const unsigned char *alpn = nullptr;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn_len != 2 || memcmp(alpn, "h2", 2) != 0) {
        ASTER_LOG_WARN("asternet-h2", "ALPN negotiation failed: expected=h2 actual=%.*s",
                   (int)alpn_len, alpn ? reinterpret_cast<const char *>(alpn) : "");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_PROTOCOL;
        return ASTERNET_ERR_PROTOCOL;
    }

    // 3. nghttp2 session
    H2Ctx h2ctx;
    h2ctx.ssl = ssl;
    h2ctx.req_body = req.body;
    h2ctx.max_response_body_bytes = req.max_response_body_bytes;
    h2ctx.deadline_ms = deadline_ms;

    nghttp2_session_callbacks *cbs = nullptr;
    if (nghttp2_session_callbacks_new(&cbs) != 0) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_OUT_OF_MEMORY;
        return ASTERNET_ERR_OUT_OF_MEMORY;
    }
    nghttp2_session_callbacks_set_send_callback(cbs, send_callback);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close_callback);

    nghttp2_session *session = nullptr;
    if (nghttp2_session_client_new(&session, cbs, &h2ctx) != 0) {
        nghttp2_session_callbacks_del(cbs);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = ASTERNET_ERR_OUT_OF_MEMORY;
        return ASTERNET_ERR_OUT_OF_MEMORY;
    }
    nghttp2_session_callbacks_del(cbs);

    // 4. HTTP/2 client preface 必须在 magic 后发送首个 SETTINGS 帧。
    int settings_ret = nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);
    if (settings_ret != 0) {
        nghttp2_session_del(session);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        resp.err_code = settings_ret == NGHTTP2_ERR_NOMEM
                            ? ASTERNET_ERR_OUT_OF_MEMORY
                            : ASTERNET_ERR_PROTOCOL;
        return resp.err_code;
    }

    // 5. 构造请求头并 submit
    std::vector<nghttp2_nv> nvs;
    auto add_nv = [&](const char *n, const char *v) {
        nghttp2_nv nv;
        nv.name = (uint8_t *)const_cast<char *>(n);
        nv.namelen = strlen(n);
        nv.value = (uint8_t *)const_cast<char *>(v);
        nv.valuelen = strlen(v);
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        nvs.push_back(nv);
    };
    add_nv(":method", req.method.c_str());
    add_nv(":scheme", "https");
    // :authority 非默认端口需含端口号（RFC 7540 §8.1.2.3）
    h2ctx.authority = req.host;
    if (req.port != 443) { h2ctx.authority += ":" + std::to_string(req.port); }
    add_nv(":authority", h2ctx.authority.c_str());
    add_nv(":path", req.path.c_str());
    add_nv("user-agent", "asternet/0.1");
    for (const auto &h : req.headers) add_nv(h.name.c_str(), h.value.c_str());

    nghttp2_data_provider data_prd{};
    data_prd.read_callback = data_read_callback;
    int32_t stream_id = nghttp2_submit_request(session, nullptr, nvs.data(), nvs.size(),
                                               req.body.empty() ? nullptr : &data_prd, &h2ctx);
    ASTER_LOG_INFO("asternet-h2", "  submit_request stream_id=%d", (int)stream_id);
    if (stream_id < 0) {
        nghttp2_session_del(session); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
        resp.err_code = ASTERNET_ERR_PROTOCOL; return ASTERNET_ERR_PROTOCOL;
    }
    h2ctx.stream_id = stream_id;

    // 6. select 驱动收发循环：非阻塞 socket 下发送前需等可写，避免 WOULDBLOCK 丢数据
    int ret = ASTERNET_OK;
    int iter = 0;
    bool got_first_byte = false;
    while (!h2ctx.stream_closed) {
        ++iter;
        // -- 发送阶段：无条件调用，send_callback 已内置重试，不会 WOULDBLOCK --
        // 关键：不依赖 want_write() 判断——nghttp2 内部队列有数据就会调 send_callback。
        {
            int sr = nghttp2_session_send(session);
            if (sr < 0) { ret = ASTERNET_ERR_PROTOCOL; break; }
        }
        if (!h2ctx.pending_data.empty()) {
            ssize_t consumed = nghttp2_session_mem_recv(
                session, reinterpret_cast<const uint8_t *>(h2ctx.pending_data.data()),
                h2ctx.pending_data.size());
            if (consumed < 0) { ret = ASTERNET_ERR_PROTOCOL; break; }
            h2ctx.pending_data.erase(0, static_cast<size_t>(consumed));
        }
        if (h2ctx.stream_closed) break;

        // -- 接收阶段：等 socket 可读（1s 超时以检测 deadline） --
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            const int64_t remaining_ms = deadline_ms - monotonic_ms();
            if (remaining_ms <= 0) { ret = ASTERNET_ERR_TIMEOUT; break; }
            struct timeval rtv{static_cast<time_t>(remaining_ms / 1000),
                               static_cast<suseconds_t>((remaining_ms % 1000) * 1000)};
            int sel = select(fd + 1, &rfds, nullptr, nullptr, &rtv);
            if (monotonic_ms() >= deadline_ms) { ret = ASTERNET_ERR_TIMEOUT; break; }
            if (sel <= 0) continue;

            uint8_t rbuf[16384];
            int n;
            for (;;) {
                n = SSL_read(ssl, rbuf, sizeof(rbuf));
                if (n >= 0) break;
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ) break;
                if (err == SSL_ERROR_WANT_WRITE) {
                    if (select_with_deadline(fd, false, true, deadline_ms) > 0) continue;
                    ret = ASTERNET_ERR_TIMEOUT;
                    n = -1;
                }
                break;
            }
            if (n > 0) {
                if (!got_first_byte) {
                    resp.ttfb_ms = monotonic_ms() - t_start;
                    got_first_byte = true;
                }
                ssize_t consumed = nghttp2_session_mem_recv(session, rbuf, n);
                ASTER_LOG_DEBUG("asternet-h2", "  iter=%d read=%d consumed=%zd got_status=%d stream_closed=%d",
                            iter, n, consumed, (int)h2ctx.got_status, (int)h2ctx.stream_closed);
                if (consumed < 0) { ret = ASTERNET_ERR_PROTOCOL; break; }
                if (consumed < n) {
                    h2ctx.pending_data.append(reinterpret_cast<const char *>(rbuf + consumed),
                                              static_cast<size_t>(n - consumed));
                }
            } else if (n == 0) {
                ASTER_LOG_INFO("asternet-h2", "  iter=%d TLS_CLOSE got_status=%d stream_closed=%d",
                           iter, (int)h2ctx.got_status, (int)h2ctx.stream_closed);
                if (!h2ctx.stream_closed) ret = ASTERNET_ERR_PROTOCOL;
                break;
            } else {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ) continue;
                ASTER_LOG_INFO("asternet-h2", "  iter=%d SSL_ERR=%d got_status=%d stream_closed=%d",
                            iter, err, (int)h2ctx.got_status, (int)h2ctx.stream_closed);
                if (!h2ctx.stream_closed) ret = ASTERNET_ERR_PROTOCOL;
                break;
            }
        }
    }

    if (h2ctx.body_limit_exceeded) {
        ret = ASTERNET_ERR_BUFFER_TOO_SMALL;
    } else if (ret == ASTERNET_OK && (!h2ctx.got_status || !h2ctx.got_end_stream
                                       || h2ctx.stream_error != NGHTTP2_NO_ERROR)) {
        ret = ASTERNET_ERR_PROTOCOL;
    }

    resp.http_status = h2ctx.http_status;
    resp.body = std::move(h2ctx.resp_body);

    nghttp2_session_del(session);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);

    resp.total_ms = monotonic_ms() - t_start;

    ASTER_LOG_INFO("asternet-h2", "<== ret=%d status=%d body_len=%zu total_ms=%lld",
               ret, resp.http_status, resp.body.size(), (long long)resp.total_ms);
    resp.err_code = ret;
    if (ret != ASTERNET_OK) {
        resp.failure_stage = ret == ASTERNET_ERR_TIMEOUT ? "timeout"
            : ret == ASTERNET_ERR_TLS ? "tls"
            : ret == ASTERNET_ERR_CONNECT ? "connect" : "protocol";
    }
    return ret;
}

}  // namespace engine
}  // namespace asternet

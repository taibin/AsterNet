/*
 * AsterNet 网络核心 —— XQUIC 传输引擎封装实现（阶段 1：单请求同步）
 *
 * 基于 test_h3_get.cpp 验证过的流程，封装为 QuicEngine 类，用 platform::EventLoop
 * 驱动（kqueue/epoll 跨平台）。
 *
 * 仅在 ASTERNET_ENABLE_XQUIC 时编译（CMake 控制），链接 xquic-static + boringssl。
 */
#include "engine/quic_engine.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "platform/log.h"
#include "platform/tls.h"

namespace asternet {
namespace engine {

namespace {
xqc_usec_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (xqc_usec_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

int64_t monotonic_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool parse_http_status(const xqc_http_header_t &header, int *status) {
    static constexpr char kStatusName[] = ":status";
    if (header.name.iov_len != sizeof(kStatusName) - 1
        || std::memcmp(header.name.iov_base, kStatusName, sizeof(kStatusName) - 1) != 0
        || header.value.iov_len != 3) {
        return false;
    }

    const auto *value = static_cast<const unsigned char *>(header.value.iov_base);
    int parsed = 0;
    for (size_t i = 0; i < header.value.iov_len; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        parsed = parsed * 10 + (value[i] - '0');
    }
    if (parsed < 100 || parsed > 599) return false;
    *status = parsed;
    return true;
}
}  // namespace

QuicEngine::QuicEngine(bool allow_insecure_tls_for_testing, std::string ca_cert_pem)
    : loop_(platform::create_event_loop()),
      allow_insecure_tls_for_testing_(allow_insecure_tls_for_testing),
      ca_cert_pem_(std::move(ca_cert_pem)) {}

QuicEngine::~QuicEngine() {
    std::lock_guard<std::mutex> lock(request_mutex_);
    if (cur_req_ && cur_req_->sock_fd >= 0) close(cur_req_->sock_fd);
    if (engine_ != nullptr) xqc_engine_destroy(engine_);
}

int QuicEngine::init_engine() {
    if (initialized_) return 0;

    xqc_config_t egn_cfg;
    if (xqc_engine_get_default_config(&egn_cfg, XQC_ENGINE_CLIENT) < 0) return -1;

    xqc_engine_ssl_config_t ssl_cfg = {};
    ssl_cfg.ciphers = const_cast<char *>(XQC_TLS_CIPHERS);
    ssl_cfg.groups = const_cast<char *>(XQC_TLS_GROUPS);

    xqc_engine_callback_t callback = {};
    callback.set_event_timer = QuicEngine::set_event_timer_cb;
    callback.log_callbacks.xqc_log_write_err = QuicEngine::log_write_err_cb;

    xqc_transport_callbacks_t transport_cbs = {};
    transport_cbs.write_socket = QuicEngine::write_socket_cb;
    transport_cbs.save_token = QuicEngine::save_token_cb;
    transport_cbs.save_session_cb = QuicEngine::save_session_cb;
    transport_cbs.save_tp_cb = QuicEngine::save_tp_cb;
    transport_cbs.cert_verify_cb = QuicEngine::cert_verify_cb;
    transport_cbs.conn_update_cid_notify = QuicEngine::conn_update_cid_notify_cb;

    engine_ = xqc_engine_create(XQC_ENGINE_CLIENT, &egn_cfg, &ssl_cfg,
                                &callback, &transport_cbs, this);
    if (engine_ == nullptr) return -1;

    xqc_h3_callbacks_t h3_cbs = {};
    h3_cbs.h3c_cbs.h3_conn_create_notify = QuicEngine::h3_conn_create_notify_cb;
    h3_cbs.h3c_cbs.h3_conn_close_notify = QuicEngine::h3_conn_close_notify_cb;
    h3_cbs.h3c_cbs.h3_conn_handshake_finished = QuicEngine::h3_conn_handshake_finished_cb;
    h3_cbs.h3r_cbs.h3_request_create_notify = QuicEngine::h3_request_create_notify_cb;
    h3_cbs.h3r_cbs.h3_request_read_notify = QuicEngine::h3_request_read_notify_cb;
    h3_cbs.h3r_cbs.h3_request_write_notify = QuicEngine::h3_request_write_notify_cb;
    h3_cbs.h3r_cbs.h3_request_closing_notify = QuicEngine::h3_request_closing_notify_cb;
    h3_cbs.h3r_cbs.h3_request_close_notify = QuicEngine::h3_request_close_notify_cb;
    if (xqc_h3_ctx_init(engine_, &h3_cbs) < 0) {
        xqc_engine_destroy(engine_);
        engine_ = nullptr;
        return -1;
    }
    // 注意：xqc_h3_ctx_init 内部已 register_alpn("h3", 内部 H3 回调)，
    // 用户 h3c_cbs/h3r_cbs 由 xquic 内部回调转发。切勿再 register_alpn 覆盖。

    initialized_ = true;
    return 0;
}

int QuicEngine::prefetch(const std::string & /*host*/) {
    // TODO(阶段2): 预建连 + 0-RTT（需 session/tp 持久化）
    return 0;
}

int QuicEngine::migrate_connection() {
    // TODO(阶段3): QUIC Connection ID 迁移
    return ASTERNET_ERR_UNSUPPORTED;
}

void QuicEngine::complete_response_if_ready(RequestContext &ctx) {
    if (!ctx.response_fin.load()) return;
    if (!ctx.got_status.load()) {
        ASTER_LOG_WARN("asternet-h3", "response finished without a valid :status header");
        ctx.failed.store(true);
        return;
    }
    if (!ctx.failed.load()) ctx.finished.store(true);
}

void QuicEngine::send_pending_body(QuicEngine *self) {
    if (self == nullptr || self->cur_req_ == nullptr || self->engine_ == nullptr) return;
    RequestContext &ctx = *self->cur_req_;
    if (ctx.cleanup_started.load() || ctx.h3_request == nullptr || ctx.body.empty()) return;

    while (ctx.request_body_sent < ctx.body.size()) {
        const size_t remaining = ctx.body.size() - ctx.request_body_sent;
        auto *data = reinterpret_cast<unsigned char *>(&ctx.body[ctx.request_body_sent]);
        const ssize_t sent = xqc_h3_request_send_body(ctx.h3_request, data, remaining, 1);
        if (sent == -XQC_EAGAIN) return;
        if (sent <= 0 || static_cast<size_t>(sent) > remaining) {
            ASTER_LOG_WARN("asternet-h3", "send request body failed ret=%zd remaining=%zu", sent, remaining);
            ctx.failed.store(true);
            return;
        }
        ctx.request_body_sent += static_cast<size_t>(sent);
    }
}

int QuicEngine::request(const Request &req, Response &resp) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    const int64_t request_start_ms = monotonic_ms();
    auto fail_early = [&](int err) {
        resp.err_code = err;
        resp.total_ms = monotonic_ms() - request_start_ms;
        return err;
    };

    resp = Response{};
    resp.protocol = ASTERNET_PROTOCOL_HTTP_3;
    ASTER_LOG_INFO("asternet-h3", "==> %s:%d %s %s timeout_ms=%d",
               req.host.c_str(), req.port, req.method.c_str(), req.path.c_str(), req.timeout_ms);
    if (init_engine() < 0) {
        return fail_early(ASTERNET_ERR_INTERNAL);
    }

    // 1. DNS 解析
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;  // 同时支持 IPv4/IPv6，兼容纯 IPv6+NAT64 网络
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", req.port);
    if (getaddrinfo(req.host.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
        if (res != nullptr) freeaddrinfo(res);
        return fail_early(ASTERNET_ERR_DNS);
    }
    struct sockaddr_storage srv_addr;
    socklen_t srv_addr_len = res->ai_addrlen;
    memcpy(&srv_addr, res->ai_addr, res->ai_addrlen);
    int addr_family = res->ai_family;
    freeaddrinfo(res);

    // 2. 创建非阻塞 UDP socket + connect（使用解析到的地址族）
    int sock_fd = socket(addr_family, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        return fail_early(ASTERNET_ERR_INTERNAL);
    }
    int flags = fcntl(sock_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock_fd);
        return fail_early(ASTERNET_ERR_INTERNAL);
    }
    if (connect(sock_fd, (struct sockaddr *)&srv_addr, srv_addr_len) < 0) {
        close(sock_fd);
        return fail_early(ASTERNET_ERR_CONNECT);
    }

    // 3. 构造请求上下文
    cur_req_ = std::make_unique<RequestContext>();
    cur_req_->host = req.host;
    cur_req_->authority = req.host;
    if (req.port != 443) cur_req_->authority += ":" + std::to_string(req.port);
    cur_req_->port = req.port;
    cur_req_->path = req.path;
    cur_req_->method = req.method;
    cur_req_->body = req.body;
    cur_req_->ca_cert_pem = req.ca_cert_pem.empty() ? ca_cert_pem_ : req.ca_cert_pem;
    cur_req_->headers = req.headers;
    cur_req_->max_response_body_bytes = req.max_response_body_bytes;
    cur_req_->allow_insecure_tls_for_testing = req.allow_insecure_tls_for_testing;
    cur_req_->start_ms = request_start_ms;
    cur_req_->generation = ++request_generation_;
    cur_req_->sock_fd = sock_fd;
    cur_req_->peer_addr = srv_addr;
    cur_req_->peer_addrlen = srv_addr_len;

    // 4. 注册 socket 可读到 EventLoop：recvfrom → xqc_engine_packet_process 喂包 → finish_recv
    loop_->add_fd(sock_fd, platform::EventLoop::kReadable, [this](int events) {
        RequestContext *ctx = cur_req_.get();
        if (ctx == nullptr || engine_ == nullptr || ctx->cleanup_started.load()) return;
        if (events & platform::EventLoop::kWritable) {
            ctx->waiting_writable.store(false);
            loop_->mod_fd(ctx->sock_fd, platform::EventLoop::kReadable);
            const xqc_int_t continue_ret = xqc_conn_continue_send(engine_, &ctx->cid);
            if (continue_ret != XQC_OK) {
                ASTER_LOG_WARN("asternet-h3", "continue UDP send ret=%d", continue_ret);
                ctx->failed.store(true);
                return;
            }
        }
        if (!(events & platform::EventLoop::kReadable)) return;
        unsigned char buf[65535];
        bool any = false;
        for (;;) {
            struct sockaddr_storage peer_addr{};
            socklen_t peer_addrlen = sizeof(peer_addr);
            ssize_t r = recvfrom(ctx->sock_fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&peer_addr, &peer_addrlen);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    ASTER_LOG_WARN("asternet-h3", "recvfrom failed errno=%d", errno);
                    ctx->failed.store(true);
                }
                break;
            }
            if (r == 0) continue;
            if (!ctx->got_local) {
                socklen_t l = sizeof(ctx->local_addr);
                if (getsockname(ctx->sock_fd, (struct sockaddr *)&ctx->local_addr, &l) != 0) {
                    ASTER_LOG_WARN("asternet-h3", "getsockname failed errno=%d", errno);
                    ctx->failed.store(true);
                    break;
                }
                ctx->local_addrlen = l;
                ctx->got_local = true;
            }
            const xqc_int_t process_ret = xqc_engine_packet_process(
                engine_, buf, static_cast<size_t>(r),
                (struct sockaddr *)&ctx->local_addr, ctx->local_addrlen,
                (struct sockaddr *)&peer_addr, peer_addrlen, now_us(), this);
            if (process_ret != XQC_OK) {
                ASTER_LOG_WARN("asternet-h3", "packet_process ret=%d", process_ret);
            }
            any = true;
        }
        if (any && engine_ != nullptr && cur_req_.get() == ctx && !ctx->cleanup_started.load()) {
            xqc_engine_finish_recv(engine_);
        }
    });

    // 5. 建连
    xqc_conn_settings_t conn_settings = xqc_conn_get_conn_settings_template(XQC_CONN_SETTINGS_DEFAULT);
    conn_settings.enable_multipath = 0;   // 关闭 multipath（POC 单路径，避免协商触发 null 回调）
    conn_settings.multipath_version = (xqc_multipath_version_t)0;
    xqc_conn_ssl_config_t conn_ssl_cfg = {};
    if (!allow_insecure_tls_for_testing_ && !cur_req_->allow_insecure_tls_for_testing) {
        conn_ssl_cfg.cert_verify_flag |= XQC_TLS_CERT_FLAG_NEED_VERIFY;
    }
    const xqc_cid_t *cid = xqc_h3_connect(engine_, &conn_settings, nullptr, 0,
                                          req.host.c_str(), 0, &conn_ssl_cfg,
                                          (struct sockaddr *)&srv_addr, srv_addr_len, this);
    if (cid == nullptr) {
        loop_->remove_fd(sock_fd);
        close(sock_fd);
        cur_req_.reset();
        return fail_early(ASTERNET_ERR_CONNECT);
    }
    cur_req_->cid = *cid;

    // 6. 事件循环驱动至完成/超时
    const int timeout_ms = req.timeout_ms > 0 ? req.timeout_ms : 15000;
    bool timed_out = false;
    while (!cur_req_->finished.load() && !cur_req_->failed.load()) {
        const int64_t elapsed_ms = monotonic_ms() - request_start_ms;
        if (elapsed_ms >= timeout_ms) {
            timed_out = true;
            break;
        }
        const int64_t remaining_ms = timeout_ms - elapsed_ms;
        loop_->poll_once(static_cast<int>(remaining_ms > 100 ? 100 : remaining_ms));
    }

    // 7. 收集结果
    RequestContext &ctx = *cur_req_;
    resp.http_status = ctx.http_status;
    resp.ttfb_ms = ctx.ttfb_ms;
    resp.total_ms = monotonic_ms() - request_start_ms;
    const size_t body_len = ctx.response_body.size();
    const bool response_complete = ctx.finished.load() && ctx.got_status.load()
                                && ctx.response_fin.load() && !ctx.failed.load();
    int ret;
    if (response_complete) {
        resp.err_code = ASTERNET_OK;
        resp.body = std::move(ctx.response_body);
        ret = ASTERNET_OK;
    } else if (ctx.tls_failed) {
        resp.err_code = ASTERNET_ERR_TLS;
        ret = ASTERNET_ERR_TLS;
    } else if (ctx.body_limit_exceeded) {
        resp.err_code = ASTERNET_ERR_BUFFER_TOO_SMALL;
        ret = ASTERNET_ERR_BUFFER_TOO_SMALL;
    } else if (timed_out) {
        resp.err_code = ASTERNET_ERR_TIMEOUT;
        ret = ASTERNET_ERR_TIMEOUT;
    } else {
        if (!ctx.failed.load()) {
            ASTER_LOG_WARN("asternet-h3", "request ended without a complete HTTP response");
        }
        resp.err_code = ASTERNET_ERR_PROTOCOL;
        ret = ASTERNET_ERR_PROTOCOL;
    }

    // 8. 当前为单请求 POC：销毁引擎以同步释放连接和定时器，隔离下一次请求。
    ctx.cleanup_started.store(true);
    xqc_engine_t *engine = engine_;
    engine_ = nullptr;
    initialized_ = false;
    if (engine != nullptr) {
        xqc_engine_destroy(engine);
    }
    loop_->remove_fd(sock_fd);
    close(sock_fd);

    ASTER_LOG_INFO("asternet-h3", "<== ret=%d status=%d body_len=%zu ttfb_ms=%lld total_ms=%lld stream_err=%d",
               ret, resp.http_status, body_len, static_cast<long long>(resp.ttfb_ms),
               static_cast<long long>(resp.total_ms), ctx.stream_error);
    cur_req_.reset();
    return ret;
}

/* ---------------- xquic transport 回调 ---------------- */

ssize_t QuicEngine::write_socket_cb(const unsigned char *buf, size_t size,
                                    const struct sockaddr * /*peer*/, socklen_t /*peer_len*/,
                                    void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self == nullptr || self->cur_req_ == nullptr || self->cur_req_->cleanup_started.load()) {
        return XQC_SOCKET_ERROR;
    }
    ssize_t n;
    do {
        n = send(self->cur_req_->sock_fd, buf, size, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        RequestContext &ctx = *self->cur_req_;
        if (!ctx.waiting_writable.exchange(true)) {
            self->loop_->mod_fd(ctx.sock_fd,
                                platform::EventLoop::kReadable | platform::EventLoop::kWritable);
        }
        return XQC_SOCKET_EAGAIN;
    }
    if (n < 0) return XQC_SOCKET_ERROR;
    return n;
}

void QuicEngine::set_event_timer_cb(xqc_usec_t wake_after, void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self == nullptr || self->cur_req_ == nullptr) return;
    // XQUIC 使用微秒，EventLoop 使用毫秒；向上取整避免短定时器变成 0。
    const uint64_t delay_ms = std::max<uint64_t>(1, (wake_after + 999) / 1000);
    const uint64_t generation = self->cur_req_->generation;
    self->loop_->schedule_timer(delay_ms, [self, generation]() {
        if (self->engine_ != nullptr && self->cur_req_ != nullptr
            && self->cur_req_->generation == generation
            && !self->cur_req_->cleanup_started.load()) {
            xqc_engine_main_logic(self->engine_);
        }
    });
}

void QuicEngine::save_token_cb(const unsigned char * /*token*/, uint32_t /*token_len*/,
                               void * /*user_data*/) {
    // POC 暂不持久化地址验证 token，但必须提供非空回调给 XQUIC。
}

void QuicEngine::save_session_cb(const char * /*data*/, size_t /*len*/, void * /*user_data*/) {
    // TODO(阶段3): 持久化 TLS session（0-RTT 复用）
}

void QuicEngine::save_tp_cb(const char * /*data*/, size_t /*len*/, void * /*user_data*/) {
    // TODO(阶段3): 持久化 transport parameters（0-RTT 复用）
}

void QuicEngine::conn_update_cid_notify_cb(xqc_connection_t * /*conn*/,
                                           const xqc_cid_t * /*retire_cid*/,
                                           const xqc_cid_t * /*new_cid*/,
                                           void * /*user_data*/) {
    // 当前请求使用已 connect 的 UDP socket，无需额外维护 CID 映射。
}

void QuicEngine::log_write_err_cb(xqc_log_level_t /*lvl*/, const void *buf, size_t size,
                                   void * /*user_data*/) {
    (void)fwrite(buf, 1, size, stderr);
}

int QuicEngine::cert_verify_cb(const unsigned char *certs[], const size_t cert_len[],
                               size_t certs_len, void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self != nullptr && self->cur_req_ != nullptr
        && (self->allow_insecure_tls_for_testing_
            || self->cur_req_->allow_insecure_tls_for_testing)) return 0;
    if (self != nullptr && self->cur_req_ != nullptr
        && asternet::platform::verify_certificate_chain(self->cur_req_->ca_cert_pem,
                                                         self->cur_req_->host,
                                                         certs, cert_len, certs_len)) {
        return 0;
    }
    if (self != nullptr && self->cur_req_ != nullptr) {
        self->cur_req_->tls_failed.store(true);
    }
    ASTER_LOG_WARN("asternet-h3", "certificate verification failed");
    return -1;
}

/* ---------------- xquic 连接回调 ---------------- */

int QuicEngine::conn_create_notify_cb(xqc_connection_t * /*conn*/, const xqc_cid_t * /*cid*/,
                                      void * /*user_data*/, void * /*conn_proto_data*/) {
    return XQC_OK;  // QUIC 级 stub，cid 由 h3_conn_create_notify 取
}

void QuicEngine::conn_handshake_finished_cb(xqc_connection_t * /*conn*/, void * /*user_data*/,
                                           void * /*conn_proto_data*/) {
    // QUIC 级握手完成 stub，H3 请求在 h3_conn_handshake_finished 发
}

int QuicEngine::conn_close_notify_cb(xqc_connection_t * /*conn*/, const xqc_cid_t * /*cid*/,
                                     void *user_data, void * /*conn_proto_data*/) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self != nullptr && self->cur_req_ != nullptr && !self->cur_req_->cleanup_started.load()
        && !self->cur_req_->finished.load()) {
        ASTER_LOG_WARN("asternet-h3", "QUIC connection closed before response completion");
        self->cur_req_->failed.store(true);
    }
    return XQC_OK;
}

xqc_int_t QuicEngine::stream_read_notify_cb(xqc_stream_t * /*s*/, void * /*user_data*/) {
    return XQC_OK;  // H3 模式由 h3_request 回调管理
}
xqc_int_t QuicEngine::stream_write_notify_cb(xqc_stream_t * /*s*/, void * /*user_data*/) {
    return XQC_OK;
}
xqc_int_t QuicEngine::stream_create_notify_cb(xqc_stream_t * /*s*/, void * /*user_data*/) {
    return XQC_OK;
}
xqc_int_t QuicEngine::stream_close_notify_cb(xqc_stream_t * /*s*/, void * /*user_data*/) {
    return XQC_OK;
}

/* ---------------- H3 连接回调（发请求在 handshake_finished） ---------------- */

int QuicEngine::h3_conn_create_notify_cb(xqc_h3_conn_t * /*h3_conn*/, const xqc_cid_t *cid,
                                        void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self && self->cur_req_) self->cur_req_->cid = *cid;
    return XQC_OK;
}

void QuicEngine::h3_conn_handshake_finished_cb(xqc_h3_conn_t * /*h3_conn*/, void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self == nullptr || self->cur_req_ == nullptr || self->engine_ == nullptr
        || self->cur_req_->cleanup_started.load()) {
        return;
    }
    RequestContext &ctx = *self->cur_req_;
    if (ctx.h3_request != nullptr) return;

    // 创建 H3 请求流（settings 需非空，参考 mini_client）
    xqc_stream_settings_t settings = {};
    ctx.h3_request = xqc_h3_request_create(self->engine_, &ctx.cid, &settings, self);
    if (ctx.h3_request == nullptr) {
        ASTER_LOG_WARN("asternet-h3", "xqc_h3_request_create failed");
        ctx.failed.store(true);
        return;
    }

    // 构造请求头
    std::vector<xqc_http_header_t> hdrs;
    auto add = [&](const char *name, const char *value, size_t value_len) {
        xqc_http_header_t h{};
        h.name.iov_base = const_cast<char *>(name);
        h.name.iov_len = std::strlen(name);
        h.value.iov_base = const_cast<char *>(value);
        h.value.iov_len = value_len;
        h.flags = XQC_HTTP_HEADER_FLAG_NONE;
        hdrs.push_back(h);
    };
    auto add_string = [&](const char *name, const std::string &value) {
        add(name, value.data(), value.size());
    };
    add_string(":method", ctx.method);
    add(":scheme", "https", sizeof("https") - 1);
    add_string(":authority", ctx.authority);
    add_string(":path", ctx.path);
    add("user-agent", "asternet/0.1", sizeof("asternet/0.1") - 1);
    for (const Header &header : ctx.headers) {
        add(header.name.c_str(), header.value.data(), header.value.size());
    }

    xqc_http_headers_t headers{};
    headers.headers = hdrs.data();
    headers.count = hdrs.size();

    const uint8_t fin = ctx.body.empty() ? 1 : 0;
    const ssize_t ret = xqc_h3_request_send_headers(ctx.h3_request, &headers, fin);
    if (ret < 0) {
        ASTER_LOG_WARN("asternet-h3", "send request headers failed ret=%zd", ret);
        ctx.failed.store(true);
        return;
    }
    if (!ctx.body.empty()) {
        send_pending_body(self);
    }
}

int QuicEngine::h3_conn_close_notify_cb(xqc_h3_conn_t *h3_conn, const xqc_cid_t * /*cid*/,
                                       void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self != nullptr && self->cur_req_ != nullptr && !self->cur_req_->cleanup_started.load()
        && !self->cur_req_->finished.load()) {
        const xqc_int_t err = xqc_h3_conn_get_errno(h3_conn);
        self->cur_req_->stream_error = err;
        ASTER_LOG_WARN("asternet-h3", "H3 connection closed before response completion err=%d", err);
        self->cur_req_->failed.store(true);
    }
    return XQC_OK;
}

/* ---------------- xquic H3 请求回调 ---------------- */

int QuicEngine::h3_request_create_notify_cb(xqc_h3_request_t * /*req*/, void * /*user_data*/) {
    return XQC_OK;
}

int QuicEngine::h3_request_read_notify_cb(xqc_h3_request_t *req,
                                           xqc_request_notify_flag_t flag, void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self == nullptr || self->cur_req_ == nullptr || self->cur_req_->cleanup_started.load()) {
        return XQC_OK;
    }
    RequestContext &ctx = *self->cur_req_;

    auto consume_headers = [&](bool is_initial) -> bool {
        uint8_t fin = 0;
        xqc_http_headers_t *headers = xqc_h3_request_recv_headers(req, &fin);
        if (headers == nullptr) {
            ASTER_LOG_WARN("asternet-h3", "receive response headers failed");
            ctx.failed.store(true);
            return false;
        }

        if (is_initial) {
            bool found_status = false;
            for (size_t i = 0; i < headers->count; ++i) {
                const xqc_http_header_t &header = headers->headers[i];
                static constexpr char kStatusName[] = ":status";
                const bool is_status = header.name.iov_base != nullptr
                                    && header.name.iov_len == sizeof(kStatusName) - 1
                                    && std::memcmp(header.name.iov_base, kStatusName,
                                                   sizeof(kStatusName) - 1) == 0;
                if (!is_status) continue;
                int status = 0;
                if (found_status || !parse_http_status(header, &status)) {
                    ASTER_LOG_WARN("asternet-h3", "invalid response :status header");
                    ctx.failed.store(true);
                    return false;
                }
                found_status = true;
                ctx.http_status = status;
            }
            if (!found_status) {
                ASTER_LOG_WARN("asternet-h3", "response is missing :status header");
                ctx.failed.store(true);
                return false;
            }
            ctx.got_status.store(true);
            if (ctx.ttfb_ms < 0) ctx.ttfb_ms = monotonic_ms() - ctx.start_ms;
            ASTER_LOG_INFO("asternet-h3", "response headers status=%d fin=%d", ctx.http_status, fin);
        }

        if (fin) {
            ctx.response_fin.store(true);
            complete_response_if_ready(ctx);
        }
        return true;
    };

    if ((flag & XQC_REQ_NOTIFY_READ_HEADER) && !consume_headers(true)) return XQC_ERROR;

    if (flag & XQC_REQ_NOTIFY_READ_BODY) {
        unsigned char buf[8192];
        bool body_fin = false;
        for (;;) {
            uint8_t fin = 0;
            const ssize_t n = xqc_h3_request_recv_body(req, buf, sizeof(buf), &fin);
            if (n == -XQC_EAGAIN) break;
            if (n < 0) {
                ASTER_LOG_WARN("asternet-h3", "receive response body failed ret=%zd", n);
                ctx.failed.store(true);
                return XQC_ERROR;
            }
            if (n > 0) {
                const size_t chunk_len = static_cast<size_t>(n);
                if (chunk_len > ctx.max_response_body_bytes
                    || ctx.response_body.size() > ctx.max_response_body_bytes - chunk_len) {
                    ASTER_LOG_WARN("asternet-h3", "response body exceeds %zu bytes",
                                   ctx.max_response_body_bytes);
                    ctx.body_limit_exceeded.store(true);
                    ctx.failed.store(true);
                    return XQC_ERROR;
                }
                ctx.response_body.append(reinterpret_cast<const char *>(buf), chunk_len);
            }
            if (fin) {
                body_fin = true;
                break;
            }
            if (n == 0) break;
        }
        if (ctx.failed.load()) return XQC_ERROR;
        if (body_fin) {
            ctx.response_fin.store(true);
            complete_response_if_ready(ctx);
        }
    }

    if ((flag & XQC_REQ_NOTIFY_READ_TRAILER) && !consume_headers(false)) return XQC_ERROR;
    if (flag & XQC_REQ_NOTIFY_READ_EMPTY_FIN) {
        ctx.response_fin.store(true);
        complete_response_if_ready(ctx);
    }
    return XQC_OK;
}

int QuicEngine::h3_request_write_notify_cb(xqc_h3_request_t * /*req*/, void *user_data) {
    send_pending_body(static_cast<QuicEngine *>(user_data));
    return XQC_OK;
}

void QuicEngine::h3_request_closing_notify_cb(xqc_h3_request_t * /*req*/, xqc_int_t err,
                                               void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self != nullptr && self->cur_req_ != nullptr && !self->cur_req_->cleanup_started.load()
        && !self->cur_req_->finished.load()) {
        self->cur_req_->stream_error = err;
        ASTER_LOG_WARN("asternet-h3", "H3 request reset by peer err=%d", err);
        self->cur_req_->failed.store(true);
    }
}

int QuicEngine::h3_request_close_notify_cb(xqc_h3_request_t *req, void *user_data) {
    auto *self = static_cast<QuicEngine *>(user_data);
    if (self == nullptr || self->cur_req_ == nullptr || self->cur_req_->cleanup_started.load()) {
        return XQC_OK;
    }
    RequestContext &ctx = *self->cur_req_;
    const xqc_request_stats_t stats = xqc_h3_request_get_stats(req);
    ctx.stream_error = stats.stream_err;
    if (stats.stream_err != 0 || !ctx.response_fin.load() || !ctx.got_status.load()) {
        ASTER_LOG_WARN("asternet-h3", "H3 request closed incomplete fin=%d status=%d stream_err=%d",
                   ctx.response_fin.load(), ctx.got_status.load(), stats.stream_err);
        ctx.failed.store(true);
    } else {
        complete_response_if_ready(ctx);
    }
    return XQC_OK;
}

}  // namespace engine
}  // namespace asternet

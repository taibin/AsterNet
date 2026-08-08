/*
 * AsterNet 网络核心 —— XQUIC 传输引擎封装（正式版）
 *
 * 基于 alibaba/xquic 提供 QUIC / HTTP/3 传输，实现 engine::NetworkEngine 接口。
 * 用 platform::EventLoop 驱动 socket I/O 与定时器，跨平台（kqueue/epoll）。
 *
 * 当前为阶段 1：单请求同步模式（request_sync），验证 H3 GET 集成。
 * 后续：网络线程 + 异步 request() + 多路复用 + 0-RTT/连接迁移完整支持。
 *
 * 依赖：xquic 静态库 + boringssl 静态库（ASTERNET_ENABLE_XQUIC 时编译）。
 */
#ifndef ASTERNET_QUIC_ENGINE_H
#define ASTERNET_QUIC_ENGINE_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"
#include "asternet/asternet.h"            // asternet_network_t
#include "platform/event_loop.h"

#include "xquic/xquic.h"
#include "xquic/xqc_http3.h"

namespace asternet {
namespace engine {

class QuicEngine : public NetworkEngine {
public:
    explicit QuicEngine(bool allow_insecure_tls_for_testing, std::string ca_cert_pem = {});
    ~QuicEngine() override;

    QuicEngine(const QuicEngine &) = delete;
    QuicEngine &operator=(const QuicEngine &) = delete;

    // ---- NetworkEngine 接口 ----
    EngineType type() const override { return EngineType::kHttp3; }
    EngineCaps caps() const override {
        EngineCaps c{};
        c.http3 = true;
        return c;
    }
    int prefetch(const std::string &host) override;
    int migrate_connection() override;

    // 同步 H3 请求：实现 NetworkEngine::request。
    // 单请求同步 H3；证书校验由平台 CA bundle 驱动，失败时严格返回错误。
    int request(const Request &req, Response &resp) override;

private:
    // 初始化 xquic 引擎 + H3 上下文（懒加载，首次 request 时调用）
    int init_engine();

    // 单请求上下文（阶段 1：一次一个请求）
    struct RequestContext {
        std::string host;
        std::string authority;
        std::string path;
        std::string method;
        std::string body;
        std::string ca_cert_pem;
        std::vector<Header> headers;
        uint16_t    port = 443;
        bool        allow_insecure_tls_for_testing = false;
        std::string response_body;
        int         sock_fd = -1;
        int         http_status = 0;
        int         stream_error = 0;
        int64_t     start_ms = 0;
        int64_t     ttfb_ms = -1;
        size_t      request_body_sent = 0;
        size_t      max_response_body_bytes = 16 * 1024 * 1024;
        uint64_t    generation = 0;
        xqc_cid_t   cid{};
        xqc_h3_request_t *h3_request = nullptr;
        std::atomic<bool> finished{false};
        std::atomic<bool> failed{false};
        std::atomic<bool> got_status{false};
        std::atomic<bool> response_fin{false};
        std::atomic<bool> body_limit_exceeded{false};
        std::atomic<bool> tls_failed{false};
        std::atomic<bool> cleanup_started{false};
        std::atomic<bool> waiting_writable{false};
        // 收包地址：peer=对端(connect 地址)，local=getsockname 取
        struct sockaddr_storage peer_addr{};
        socklen_t peer_addrlen = 0;
        struct sockaddr_storage local_addr{};
        socklen_t local_addrlen = 0;
        bool got_local = false;
    };
    std::unique_ptr<RequestContext> cur_req_;

    static void complete_response_if_ready(RequestContext &ctx);
    static void send_pending_body(QuicEngine *self);

    // ---- xquic 回调（静态，经 user_data 转发到实例）----
    static ssize_t write_socket_cb(const unsigned char *buf, size_t size,
                                   const struct sockaddr *peer, socklen_t peer_len,
                                   void *user_data);
    static void set_event_timer_cb(xqc_usec_t wake_after, void *user_data);
    static void save_token_cb(const unsigned char *token, uint32_t token_len,
                              void *user_data);
    static void save_session_cb(const char *data, size_t len, void *user_data);
    static void save_tp_cb(const char *tp_data, size_t tp_len, void *user_data);
    static void conn_update_cid_notify_cb(xqc_connection_t *conn,
                                          const xqc_cid_t *retire_cid,
                                          const xqc_cid_t *new_cid,
                                          void *user_data);
    static void log_write_err_cb(xqc_log_level_t lvl, const void *buf, size_t size,
                                  void *user_data);
    static int cert_verify_cb(const unsigned char *certs[], const size_t cert_len[],
                              size_t certs_len, void *user_data);

    // QUIC 连接回调（经 xqc_engine_register_alpn 注册，POC stub）
    static int conn_create_notify_cb(xqc_connection_t *conn, const xqc_cid_t *cid,
                                     void *user_data, void *conn_proto_data);
    static void conn_handshake_finished_cb(xqc_connection_t *conn, void *user_data,
                                           void *conn_proto_data);
    static int conn_close_notify_cb(xqc_connection_t *conn, const xqc_cid_t *cid,
                                    void *user_data, void *conn_proto_data);
    // QUIC stream 回调 stub（H3 模式下由 h3_request 回调管理）
    static xqc_int_t stream_read_notify_cb(xqc_stream_t *s, void *user_data);
    static xqc_int_t stream_write_notify_cb(xqc_stream_t *s, void *user_data);
    static xqc_int_t stream_create_notify_cb(xqc_stream_t *s, void *user_data);
    static xqc_int_t stream_close_notify_cb(xqc_stream_t *s, void *user_data);

    // H3 连接回调（发请求在 h3_conn_handshake_finished）
    static int h3_conn_create_notify_cb(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid,
                                        void *user_data);
    static void h3_conn_handshake_finished_cb(xqc_h3_conn_t *h3_conn, void *user_data);
    static int h3_conn_close_notify_cb(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid,
                                       void *user_data);

    static int h3_request_create_notify_cb(xqc_h3_request_t *req, void *user_data);
    static int h3_request_read_notify_cb(xqc_h3_request_t *req,
                                         xqc_request_notify_flag_t flag, void *user_data);
    static int h3_request_write_notify_cb(xqc_h3_request_t *req, void *user_data);
    static void h3_request_closing_notify_cb(xqc_h3_request_t *req, xqc_int_t err, void *user_data);
    static int h3_request_close_notify_cb(xqc_h3_request_t *req, void *user_data);

    xqc_engine_t *engine_ = nullptr;
    std::unique_ptr<platform::EventLoop> loop_;
    std::mutex request_mutex_;
    uint64_t request_generation_ = 0;
    bool allow_insecure_tls_for_testing_ = false;
    std::string ca_cert_pem_;
    bool initialized_ = false;
};

}  // namespace engine
}  // namespace asternet

#endif  // ASTERNET_QUIC_ENGINE_H

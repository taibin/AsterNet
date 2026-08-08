/*
 * AsterNet 网络核心 —— XQUIC HTTP/3 GET 完整 POC
 *
 * 目标：跑通真实 HTTP/3 GET，验证 xquic 端到端能力（建连 + H3 请求 + 响应读取）。
 *
 * 实现要点（参考 xquic/demo/demo_client.c）：
 *   1. 创建非阻塞 UDP socket，connect 到 server
 *   2. 创建 xquic 引擎 + 初始化 H3 上下文
 *   3. xqc_h3_connect 建连（证书校验 POC 跳过）
 *   4. kqueue 事件循环驱动：socket 可读 → recvfrom → xqc_engine_main_logic
 *   5. conn_handshake_finished 回调 → xqc_h3_request_create + send_headers 发起 GET
 *   6. h3_request_read_notify 回调 → xqc_h3_request_recv_body 读响应
 *
 * 用法：./test_h3_get <host> <path>
 *   例：./test_h3_get cloudflarequic.com /
 *       ./test_h3_get www.cloudflare.com /
 *
 * 编译：
 *   clang++ -std=c++17 -Iinclude -Ithird_party/xquic/include \
 *     -Ithird_party/xquic/third_party/boringssl/include \
 *     tests/test_h3_get.cpp \
 *     third_party/xquic/build/libxquic-static.a \
 *     third_party/xquic/third_party/boringssl/build/libssl.a \
 *     third_party/xquic/third_party/boringssl/build/libcrypto.a \
 *     -lpthread -o /tmp/test_h3_get
 */
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <string>
#include <vector>

#include "xquic/xquic.h"
#include "xquic/xqc_http3.h"

namespace {

// 全局上下文
struct H3Ctx {
    xqc_engine_t *engine = nullptr;
    int sock_fd = -1;
    int kq = -1;
    struct sockaddr_storage server_addr{};
    socklen_t server_addr_len = 0;
    std::string host;
    std::string path;
    xqc_cid_t cid{};
    xqc_h3_request_t *h3_request = nullptr;
    std::string response_body;
    int response_status = 0;
    std::atomic<bool> finished{false};
    std::atomic<bool> failed{false};
};

H3Ctx g_ctx;

// 当前时间（微秒），xquic 用 xqc_usec_t
xqc_usec_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (xqc_usec_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

// ---- transport 回调 ----
ssize_t write_socket_cb(const unsigned char *buf, size_t size,
                        const struct sockaddr *peer_addr, socklen_t peer_addrlen,
                        void * /*conn_user_data*/) {
    // socket 已 connect，直接用 send 发送（connect 后的 UDP socket sendto 传非空地址在某些平台报错）
    ssize_t n;
    do {
        n = send(g_ctx.sock_fd, buf, size, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN) return XQC_SOCKET_EAGAIN;
        std::fprintf(stderr, "[write_socket] send failed: %s (size=%zu)\n", std::strerror(errno), size);
        return XQC_SOCKET_ERROR;
    }
    return n;
}

void set_event_timer_cb(xqc_msec_t wake_after, void * /*user_data*/) {
    // 注册 kqueue 定时器，到期驱动 xqc_engine_main_loop_steps
    struct kevent kev;
    EV_SET(&kev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, wake_after, nullptr);
    kevent(g_ctx.kq, &kev, 1, nullptr, 0, nullptr);
}

void save_session_cb(const char * /*data*/, size_t /*len*/, void * /*user_data*/) {}
void save_tp_cb(const char * /*data*/, size_t /*len*/, void * /*user_data*/) {}

// ---- 引擎日志回调（简化） ----
void log_write_err(xqc_log_level_t /*lvl*/, const void *buf, size_t len, void * /*user_data*/) {
    std::string s(static_cast<const char *>(buf), len);
    std::fprintf(stderr, "[xquic][err] %s", s.c_str());
}

// ---- 连接回调 ----
int conn_create_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void * /*user_data*/,
                       void * /*conn_proto_data*/) {
    g_ctx.cid = *cid;
    return XQC_OK;
}

int conn_handshake_finished(xqc_connection_t *conn, const xqc_cid_t *cid, void * /*user_data*/) {
    std::printf("[info] handshake finished, sending H3 GET %s\n", g_ctx.path.c_str());

    // 创建 H3 请求流
    g_ctx.h3_request = xqc_h3_request_create(g_ctx.engine, cid, nullptr, nullptr);
    if (g_ctx.h3_request == nullptr) {
        std::fprintf(stderr, "FAIL: xqc_h3_request_create\n");
        g_ctx.failed = true;
        return XQC_ERROR;
    }

    // 构造请求头：:method :scheme :authority :path
    std::vector<xqc_http_header_t> hdrs;
    auto add_hdr = [&](const char *n, const char *v) {
        xqc_http_header_t h{};
        h.name.iov_base = const_cast<char *>(n);
        h.name.iov_len = std::strlen(n);
        h.value.iov_base = const_cast<char *>(v);
        h.value.iov_len = std::strlen(v);
        h.flags = XQC_HTTP_HEADER_FLAG_NONE;
        hdrs.push_back(h);
    };
    add_hdr(":method", "GET");
    add_hdr(":scheme", "https");
    add_hdr(":authority", g_ctx.host.c_str());
    add_hdr(":path", g_ctx.path.c_str());
    add_hdr("user-agent", "asternet-poc/0.1");

    xqc_http_headers_t headers{};
    headers.headers = hdrs.data();
    headers.count = hdrs.size();

    ssize_t ret = xqc_h3_request_send_headers(g_ctx.h3_request, &headers, 1);  // fin=1，GET 无 body
    if (ret < 0) {
        std::fprintf(stderr, "FAIL: xqc_h3_request_send_headers ret=%zd\n", ret);
        g_ctx.failed = true;
        return XQC_ERROR;
    }
    return XQC_OK;
}

int conn_close_notify(xqc_connection_t * /*conn*/, const xqc_cid_t * /*cid*/, void * /*user_data*/) {
    std::printf("[info] connection closed\n");
    g_ctx.finished = true;
    return XQC_OK;
}

// ---- H3 请求回调 ----
int h3_request_read_notify(xqc_h3_request_t *h3_request, xqc_request_notify_flag_t flag,
                           void * /*h3s_user_data*/) {
    if (flag & XQC_REQ_NOTIFY_READ_HEADER) {
        std::printf("[info] response headers received\n");
    }
    if (flag & XQC_REQ_NOTIFY_READ_BODY) {
        // 响应体到达，读取
        unsigned char buf[8192];
        for (;;) {
            uint8_t fin = 0;
            ssize_t n = xqc_h3_request_recv_body(h3_request, buf, sizeof(buf), &fin);
            if (n <= 0) break;
            g_ctx.response_body.append(reinterpret_cast<char *>(buf), n);
            if (fin) break;
        }
    }
    if (flag & XQC_REQ_NOTIFY_READ_TRAILER) {
        std::printf("[info] response trailers received\n");
    }
    if (flag & XQC_REQ_NOTIFY_READ_EMPTY_FIN) {
        std::printf("[info] response empty fin\n");
        g_ctx.finished = true;
    }
    return XQC_OK;
}

int h3_request_close_notify(xqc_h3_request_t * /*h3_request*/, void * /*h3s_user_data*/) {
    std::printf("[info] request closed, response body size=%zu\n", g_ctx.response_body.size());
    g_ctx.finished = true;
    return XQC_OK;
}

// 证书校验：POC 跳过（仅验证连通性，生产环境必须校验）
int cert_verify_cb(const unsigned char * /*certs*/[], const size_t /*cert_len*/[], size_t /*certs_len*/,
                   void * /*user_data*/) {
    return 0;  // 0 表示校验通过
}

}  // namespace

int main(int argc, char *argv[]) {
    // 行缓冲，确保被 kill 前输出可见
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);

    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <host> <path>\n  e.g. %s cloudflarequic.com /\n", argv[0], argv[0]);
        return 1;
    }
    g_ctx.host = argv[1];
    g_ctx.path = argv[2];

    // 1. DNS 解析 host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int err = getaddrinfo(g_ctx.host.c_str(), "443", &hints, &res);
    if (err != 0 || res == nullptr) {
        std::fprintf(stderr, "FAIL: getaddrinfo %s: %s\n", g_ctx.host.c_str(), gai_strerror(err));
        return 1;
    }
    std::memcpy(&g_ctx.server_addr, res->ai_addr, res->ai_addrlen);
    g_ctx.server_addr_len = res->ai_addrlen;
    char ip_str[INET_ADDRSTRLEN];
    auto *sin = reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
    inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
    std::printf("[info] resolved %s -> %s:443\n", g_ctx.host.c_str(), ip_str);
    freeaddrinfo(res);

    // 2. 创建非阻塞 UDP socket
    g_ctx.sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_ctx.sock_fd < 0) { std::perror("socket"); return 1; }
    fcntl(g_ctx.sock_fd, F_SETFL, O_NONBLOCK);
    if (connect(g_ctx.sock_fd, (struct sockaddr *)&g_ctx.server_addr, g_ctx.server_addr_len) < 0) {
        std::perror("connect"); return 1;
    }

    // 3. kqueue
    g_ctx.kq = kqueue();

    // 4. 创建 xquic 引擎
    xqc_config_t egn_cfg;
    xqc_engine_get_default_config(&egn_cfg, XQC_ENGINE_CLIENT);

    xqc_engine_ssl_config_t ssl_cfg = {};
    ssl_cfg.ciphers = const_cast<char *>(XQC_TLS_CIPHERS);
    ssl_cfg.groups = const_cast<char *>(XQC_TLS_GROUPS);

    xqc_engine_callback_t callback = {};
    callback.set_event_timer = set_event_timer_cb;
    callback.log_callbacks.xqc_log_write_err = log_write_err;

    xqc_transport_callbacks_t transport_cbs = {};
    transport_cbs.write_socket = write_socket_cb;
    transport_cbs.save_session_cb = save_session_cb;
    transport_cbs.save_tp_cb = save_tp_cb;
    transport_cbs.cert_verify_cb = cert_verify_cb;  // 证书校验在 transport_callbacks（POC 跳过）

    g_ctx.engine = xqc_engine_create(XQC_ENGINE_CLIENT, &egn_cfg, &ssl_cfg,
                                     &callback, &transport_cbs, nullptr);
    if (g_ctx.engine == nullptr) {
        std::fprintf(stderr, "FAIL: xqc_engine_create\n"); return 1;
    }

    // 5. 初始化 H3 上下文（注册请求回调）
    xqc_h3_callbacks_t h3_cbs = {};
    h3_cbs.h3r_cbs.h3_request_read_notify = h3_request_read_notify;
    h3_cbs.h3r_cbs.h3_request_close_notify = h3_request_close_notify;
    if (xqc_h3_ctx_init(g_ctx.engine, &h3_cbs) < 0) {
        std::fprintf(stderr, "FAIL: xqc_h3_ctx_init\n"); return 1;
    }

    // 6. 建连
    xqc_conn_settings_t conn_settings = xqc_conn_get_conn_settings_template(XQC_CONN_SETTINGS_DEFAULT);
    xqc_conn_ssl_config_t conn_ssl_cfg = {};
    const char *alpn = "h3";

    const xqc_cid_t *cid = xqc_h3_connect(g_ctx.engine, &conn_settings,
                                          nullptr, 0, g_ctx.host.c_str(), 0,
                                          &conn_ssl_cfg,
                                          (struct sockaddr *)&g_ctx.server_addr,
                                          g_ctx.server_addr_len, nullptr);
    if (cid == nullptr) {
        std::fprintf(stderr, "FAIL: xqc_h3_connect\n"); return 1;
    }
    g_ctx.cid = *cid;

    // 注册 socket 可读事件到 kqueue
    struct kevent sock_kev;
    EV_SET(&sock_kev, g_ctx.sock_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    kevent(g_ctx.kq, &sock_kev, 1, nullptr, 0, nullptr);

    // 7. 事件循环
    std::printf("[info] entering event loop...\n");
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 15;  // 15s 超时
    int idle_count = 0;

    while (!g_ctx.finished && !g_ctx.failed) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec) {
            std::fprintf(stderr, "FAIL: timeout\n");
            break;
        }

        struct kevent events[8];
        int n = kevent(g_ctx.kq, nullptr, 0, events, 8, nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("kevent"); break;
        }
        bool active = false;
        for (int i = 0; i < n; i++) {
            if (events[i].filter == EVFILT_READ && events[i].ident == (uintptr_t)g_ctx.sock_fd) {
                // 收包
                unsigned char buf[65535];
                for (;;) {
                    ssize_t r = recvfrom(g_ctx.sock_fd, buf, sizeof(buf), 0, nullptr, nullptr);
                    if (r <= 0) break;
                }
                xqc_engine_main_logic(g_ctx.engine);
                active = true;
            } else if (events[i].filter == EVFILT_TIMER) {
                // 定时器到期，驱动引擎
                xqc_engine_main_logic(g_ctx.engine);
                active = true;
            }
        }
        if (!active) {
            // 空转，主动驱动一次（保险）
            xqc_engine_main_logic(g_ctx.engine);
            if (++idle_count > 100000) { std::fprintf(stderr, "FAIL: idle spin\n"); break; }
        }
    }

    // 8. 输出结果
    if (g_ctx.failed) {
        std::fprintf(stderr, "RESULT: FAILED\n");
        return 1;
    }
    std::printf("\n===== HTTP/3 GET SUCCESS =====\n");
    std::printf("host: %s, path: %s\n", g_ctx.host.c_str(), g_ctx.path.c_str());
    std::printf("response body size: %zu bytes\n", g_ctx.response_body.size());
    if (!g_ctx.response_body.empty()) {
        std::printf("response body (first 512 bytes):\n");
        size_t show = std::min(g_ctx.response_body.size(), (size_t)512);
        std::printf("%.*s\n", (int)show, g_ctx.response_body.c_str());
    }
    std::printf("==============================\n");

    if (g_ctx.h3_request) xqc_h3_request_close(g_ctx.h3_request);
    if (g_ctx.engine) xqc_engine_destroy(g_ctx.engine);
    if (g_ctx.sock_fd >= 0) close(g_ctx.sock_fd);
    if (g_ctx.kq >= 0) close(g_ctx.kq);
    return 0;
}

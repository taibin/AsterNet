/*
 * QuicEngine 最小集成测试：验证 asternet-core 能正确链接 xquic + boringssl 静态库，
 * 并完成引擎创建 + H3 上下文初始化（不发起真实网络请求，仅验证 API 链路）。
 *
 * 这是 POC 的"集成可行性"验证：从独立编译 xquic 到嵌入 asternet-core 的关键一跃。
 *
 * 编译（macOS arm64 桌面）：
 *   clang++ -std=c++17 -Iinclude -Ithird_party/xquic/include \
 *     -Ithird_party/xquic/third_party/boringssl/include \
 *     tests/test_quic_integration.cpp \
 *     -Lthird_party/xquic/build -lxquic-static \
 *     -Lthird_party/xquic/third_party/boringssl/build -lssl -lcrypto \
 *     -lpthread -o /tmp/test_quic_integration
 */
#include <cstdio>
#include <cstring>

#include "xquic/xquic.h"
#include "xquic/xqc_http3.h"

// transport 回调 stub（按 xquic 真实签名）
// write_socket: ssize_t(buf, size, peer_addr, peer_addrlen, conn_user_data)
static ssize_t write_socket_cb(const unsigned char * /*buf*/, size_t /*size*/,
                               const struct sockaddr * /*peer_addr*/, socklen_t /*peer_addrlen*/,
                               void * /*conn_user_data*/) {
    return 0;  // POC: 不实际发送
}
static void set_event_timer_cb(xqc_msec_t /*wake_after*/, void * /*user_data*/) {
    // POC: 不驱动事件循环
}
// save_session: void(data, data_len, conn_user_data)
static void save_session_cb(const char * /*data*/, size_t /*data_len*/, void * /*conn_user_data*/) {
}
// save_tp: void(tp_data, tp_len, conn_user_data)  tp_data 实际为 const char*
static void save_tp_cb(const char * /*tp_data*/, size_t /*tp_len*/, void * /*conn_user_data*/) {
}

int main() {
    // 1. 引擎默认配置
    xqc_config_t egn_cfg;
    if (xqc_engine_get_default_config(&egn_cfg, XQC_ENGINE_CLIENT) < 0) {
        std::printf("FAIL: xqc_engine_get_default_config\n");
        return 1;
    }

    // 2. SSL 配置
    xqc_engine_ssl_config_t ssl_cfg = {};
    ssl_cfg.ciphers = const_cast<char *>(XQC_TLS_CIPHERS);
    ssl_cfg.groups = const_cast<char *>(XQC_TLS_GROUPS);

    // 3. 回调
    xqc_engine_callback_t callback = {};
    callback.set_event_timer = set_event_timer_cb;

    xqc_transport_callbacks_t transport_cbs = {};
    transport_cbs.write_socket = write_socket_cb;
    transport_cbs.save_session_cb = save_session_cb;
    transport_cbs.save_tp_cb = save_tp_cb;

    // 4. 创建客户端引擎
    xqc_engine_t *engine = xqc_engine_create(XQC_ENGINE_CLIENT, &egn_cfg, &ssl_cfg,
                                             &callback, &transport_cbs, nullptr);
    if (engine == nullptr) {
        std::printf("FAIL: xqc_engine_create\n");
        return 1;
    }

    // 5. 初始化 H3 上下文（H3 请求能力的前置）
    xqc_h3_callbacks_t h3_cbs = {};
    if (xqc_h3_ctx_init(engine, &h3_cbs) < 0) {
        std::printf("FAIL: xqc_h3_ctx_init\n");
        xqc_engine_destroy(engine);
        return 1;
    }

    std::printf("OK: xquic engine created, h3 ctx initialized\n");

    xqc_engine_destroy(engine);
    std::printf("test_quic_integration OK\n");
    return 0;
}

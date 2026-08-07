/*
 * QuicEngine 集成测试：验证 request_sync 跑通 H3 GET。
 * 需先启动 xquic test_server（见 docs/POC_PROGRESS.md 步骤 D）。
 *
 *   ./test_quic_engine
 * 预期：ret=0, body_size>0
 */
#include "engine/quic_engine.h"

#include <cstdio>

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    asternet::engine::QuicEngine engine;
    asternet::engine::QuicResponse resp;
    int ret = engine.request_sync("127.0.0.1", 8443, "GET", "/index.html", "", resp, 10000);
    std::printf("ret=%d err_code=%d body_size=%zu\n", ret, resp.err_code, resp.body.size());
    if (!resp.body.empty()) {
        std::printf("body(first 80 bytes): %.80s\n", resp.body.c_str());
    }
    return (ret == 0 && !resp.body.empty()) ? 0 : 1;
}

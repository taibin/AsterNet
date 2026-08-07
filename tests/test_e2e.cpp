/*
 * 端到端测试：经 CABI → Client → ProtocolSelector 降级链（H3→H2→H1.1）请求真实站点。
 *   ./test_e2e
 * 预期：H3 可能因 UDP/443 被封降级到 H2，最终成功（H2 或 H1.1）返回 body。
 */
#include "asternet/asternet.h"

#include <cstdio>
#include <cstring>

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    asternet_client_config_t cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = ASTERNET_ABI_VERSION;
    asternet_result_t err = ASTERNET_OK;
    asternet_client_t *c = asternet_client_create(&cfg, &err);
    if (!c || err != ASTERNET_OK) {
        std::printf("FAIL: create client err=%d\n", err);
        return 1;
    }

    char buf[4096];
    asternet_request_t request{};
    request.host = "www.cloudflare.com";
    request.port = 443;
    request.method = "GET";
    request.path = "/cdn-cgi/trace";
    request.protocol_policy = ASTERNET_POLICY_AUTO;
    request.timeout_ms = 10000;
    request.idempotent = 1;
    asternet_response_info_t info{};
    const asternet_result_t result = asternet_client_request_sync(
        c, &request, reinterpret_cast<uint8_t *>(buf), sizeof(buf), &info);
    if (result == ASTERNET_OK) {
        buf[info.body_copied < sizeof(buf) ? info.body_copied : sizeof(buf) - 1] = '\0';
        std::printf("SUCCESS: got %zu bytes via protocol %d\n", info.body_copied,
                    static_cast<int>(info.protocol));
        std::printf("body(first 100): %.100s\n", buf);
        asternet_client_destroy(c);
        return 0;
    }
    std::printf("FAIL: request ret=%d status=%d\n", result, info.http_status);
    asternet_client_destroy(c);
    return 1;
}

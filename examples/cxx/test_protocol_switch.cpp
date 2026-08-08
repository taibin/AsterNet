/*
 * 协议切换测试：对同一站点分别用 H1/H2/H3 请求，对比耗时。
 *   ./test_protocol_switch
 * 验证：可指定协议、记录耗时、返回实际协议。
 */
#include "asternet/asternet.h"

#include <cstdio>
#include <cstring>

static const char *proto_name(int p) {
    switch (p) {
        case 1: return "HTTP/1.1";
        case 2: return "HTTP/2";
        case 3: return "HTTP/3";
        default: return "AUTO";
    }
}

static void test(asternet_client_t *c, const char *host, const char *path,
                 asternet_protocol_policy_t policy) {
    char buf[2048];
    asternet_request_t request{};
    request.host = host;
    request.port = 443;
    request.method = "GET";
    request.path = path;
    request.protocol_policy = policy;
    request.timeout_ms = 12000;
    request.idempotent = 1;
    asternet_response_info_t info{};
    asternet_result_t result = asternet_client_request_sync(c, &request,
                                                            reinterpret_cast<uint8_t *>(buf),
                                                            sizeof(buf), &info);
    std::printf("[%s] ret=%d status=%d used=%s total=%lldms body=%zu\n",
                proto_name(static_cast<int>(policy)), result, info.http_status,
                proto_name(static_cast<int>(info.protocol)),
                static_cast<long long>(info.total_ms), info.body_copied);
    if (result == ASTERNET_OK) {
        buf[info.body_copied < sizeof(buf) ? info.body_copied : sizeof(buf) - 1] = '\0';
        std::printf("  body: %.80s\n", buf);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    asternet_client_config_t cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = ASTERNET_ABI_VERSION;
    asternet_result_t err = ASTERNET_OK;
    asternet_client_t *c = asternet_client_create(&cfg, &err);
    if (!c || err != ASTERNET_OK) {
        std::printf("FAIL create: err=%d\n", err);
        return 1;
    }

    const char *host = "www.cloudflare.com";
    const char *path = "/cdn-cgi/trace";
    std::printf("=== 对 %s %s 对比 H1/H2/H3/AUTO ===\n", host, path);
    test(c, host, path, ASTERNET_POLICY_HTTP_1_1_ONLY);
    test(c, host, path, ASTERNET_POLICY_HTTP_2_ONLY);
    test(c, host, path, ASTERNET_POLICY_HTTP_3_ONLY);
    test(c, host, path, ASTERNET_POLICY_AUTO);

    asternet_client_destroy(c);
    return 0;
}

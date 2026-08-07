/*
 * Http2Engine 测试：对支持 HTTP/2 的站点测 GET，验证 H2 链路。
 *   ./test_http2_engine [host] [port] [path]
 * 预期：ret=0, http_status=200, protocol=2(HTTP/2)
 */
#include "engine/http2_engine.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

int main(int argc, char *argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    asternet::engine::Http2Engine engine(false);

    asternet::engine::Request req;
    req.host = argc > 1 ? argv[1] : "www.cloudflare.com";
    if (argc > 2) {
        const char *port_arg = argv[2];
        char *end = nullptr;
        errno = 0;
        for (const char *p = port_arg; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                std::fprintf(stderr, "invalid port: %s\n", port_arg);
                return 2;
            }
        }
        unsigned long port = std::strtoul(port_arg, &end, 10);
        if (errno != 0 || end == port_arg || *end != '\0' || port == 0 || port > UINT16_MAX) {
            std::fprintf(stderr, "invalid port: %s\n", port_arg);
            return 2;
        }
        req.port = static_cast<uint16_t>(port);
    }
    req.method = "GET";
    req.path = argc > 3 ? argv[3] : "/";
    req.timeout_ms = 10000;

    asternet::engine::Response resp;
    int ret = engine.request(req, resp);
    std::printf("ret=%d err=%d http_status=%d protocol=%d body_size=%zu total_ms=%lld\n",
                ret, resp.err_code, resp.http_status, (int)resp.protocol,
                resp.body.size(), (long long)resp.total_ms);
    if (!resp.body.empty()) {
        std::printf("body(first 100): %.100s\n", resp.body.c_str());
    }
    return (ret == 0 && resp.http_status > 0) ? 0 : 1;
}

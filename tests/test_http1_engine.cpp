/*
 * Http1Engine 测试：对真实 HTTPS 站点测 GET，验证 HTTP/1.1 链路。
 *   ./test_http1_engine
 * 预期：ret=0, http_status=200/3xx, body 非空
 */
#include "engine/http1_engine.h"

#include <cstdio>

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    asternet::engine::Http1Engine engine(false);

    asternet::engine::Request req;
    req.host = "www.example.com";
    req.port = 443;
    req.method = "GET";
    req.path = "/";
    req.timeout_ms = 10000;

    asternet::engine::Response resp;
    int ret = engine.request(req, resp);
    std::printf("ret=%d err=%d http_status=%d protocol=%d body_size=%zu total_ms=%lld\n",
                ret, resp.err_code, resp.http_status, (int)resp.protocol,
                resp.body.size(), (long long)resp.total_ms);
    if (!resp.body.empty()) {
        std::printf("body(first 120): %.120s\n", resp.body.c_str());
    }
    return (ret == 0 && resp.http_status > 0) ? 0 : 1;
}

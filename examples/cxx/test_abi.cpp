/*
 * AsterNet 网络核心 —— ABI 最小冒烟测试
 * 不依赖 GoogleTest，仅用 assert 验证 client 生命周期 / 版本 / ABI 校验 / 诊断。
 */
#include "asternet/asternet.h"
#include "asternet/version.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

void quality_callback(const asternet_quality_snapshot_t *snapshot, void *user_data) {
    int *calls = static_cast<int *>(user_data);
    assert(snapshot != nullptr);
    assert(snapshot->quality >= ASTERNET_QUALITY_UNKNOWN);
    assert(snapshot->quality <= ASTERNET_QUALITY_OFFLINE);
    ++*calls;
}

}  // namespace

int main() {
    // 1. 版本字符串一致
    assert(std::strcmp(asternet_version(), ASTERNET_VERSION_STRING) == 0);

    // 2. 正常创建（ABI 版本匹配）
    asternet_client_config_t cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = ASTERNET_ABI_VERSION;
    asternet_result_t err = ASTERNET_OK;
    asternet_client_t *c = asternet_client_create(&cfg, &err);
    assert(c != nullptr);
    assert(err == ASTERNET_OK);

    // 3. 空参数
    assert(asternet_client_create(nullptr, &err) == nullptr);
    assert(err == ASTERNET_ERR_INVALID_ARGUMENT);

    // 4. ABI 主版本不匹配
    asternet_client_config_t bad{};
    bad.struct_size = sizeof(bad);
    bad.abi_version = 0xFFFF0000u;  // 主版本 0xFFFF
    asternet_result_t err2 = ASTERNET_OK;
    assert(asternet_client_create(&bad, &err2) == nullptr);
    assert(err2 == ASTERNET_ERR_ABI_VERSION);

    // 5. 诊断 dump 写入缓冲区并以 '\0' 结尾
    char buf[64];
    std::memset(buf, 0xFF, sizeof(buf));
    size_t n = asternet_client_dump_diagnostics(c, buf, sizeof(buf));
    assert(n > 0 && n < sizeof(buf));
    assert(buf[n] == '\0');

    // 6. 网络变化通知（stub 阶段不应崩溃）
    int quality_calls = 0;
    asternet_client_set_quality_callback(c, quality_callback, &quality_calls);
    assert(quality_calls == 1);
    asternet_client_on_network_change(c, ASTERNET_NETWORK_WIFI);
    assert(quality_calls == 2);

    // 7. prefetch 入口不应崩溃；物理预连接未实现时必须明确返回 UNSUPPORTED。
    asternet_result_t prefetch = asternet_client_prefetch(c, "203.0.113.20");
    assert(prefetch == ASTERNET_OK);
    assert(asternet_client_prefetch(c, "") == ASTERNET_ERR_INVALID_ARGUMENT);

    char trace[512];
    size_t trace_size = asternet_client_trace_route(c, "127.0.0.1", 443,
                                                    trace, sizeof(trace));
    assert(trace_size > 0 && trace_size < sizeof(trace));
    assert(std::strstr(trace, "\"host\":\"127.0.0.1\"") != nullptr);
    assert(std::strstr(trace, "\"hops\"") != nullptr);
    assert(quality_calls == 2);
    assert(asternet_client_trace_route(c, "", 443, trace, sizeof(trace)) == 0);

    // destroy 本身幂等，空句柄也应安全处理。
    asternet_client_destroy(c);
    asternet_client_destroy(nullptr);

    std::printf("test_abi OK\n");
    return 0;
}

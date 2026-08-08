// AsterNet HarmonyOS NAPI 桥接
// 将 C ABI 包装为 NAPI 接口，供 ArkTS 层调用。

#include <napi/native_api.h>
#include <cstring>
#include <string>
#include <vector>

#include "asternet/asternet.h"

// ---- 工具函数 ----

struct NapiCallback {
    napi_env env;
    napi_ref callback;
};

static asternet_client_t *g_client = nullptr;

// ---- NAPI: asternetCreateClient ----
static napi_value NapiCreateClient(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    bool enableH3 = true;
    if (argc > 0) {
        napi_get_value_bool(env, args[0], &enableH3);
    }

    asternet_client_config_t cfg{};
    cfg.struct_size = sizeof(cfg);
    cfg.abi_version = ASTERNET_ABI_VERSION;
    cfg.default_timeout_ms = 12000;
    cfg.max_response_body_bytes = 4 * 1024 * 1024;
    cfg.enable_http3 = enableH3 ? 1 : 0;
    cfg.allow_insecure_tls_for_testing = 0;
    cfg.ca_cert_pem = nullptr;

    asternet_result_t err = ASTERNET_OK;
    if (g_client) asternet_client_destroy(g_client);
    g_client = asternet_client_create(&cfg, &err);

    napi_value result;
    napi_create_int32(env, static_cast<int32_t>(err), &result);
    return result;
}

// ---- NAPI: asternetRequest ----
static napi_value NapiRequest(napi_env env, napi_callback_info info) {
    size_t argc = 9;
    napi_value args[9];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (!g_client) {
        napi_value ret;
        napi_create_string_utf8(env, "{\"result\":-3}", NAPI_AUTO_LENGTH, &ret);
        return ret;
    }

    // 提取参数
    char host[256] = {}, method[16] = {}, path[512] = {}, headers[4096] = {}, body[65536] = {};
    int32_t port = 443, policy = 0, timeoutMs = 12000;
    bool idempotent = true;

    napi_get_value_string_utf8(env, args[0], host, sizeof(host), nullptr);
    napi_get_value_int32(env, args[1], &port);
    napi_get_value_string_utf8(env, args[2], method, sizeof(method), nullptr);
    napi_get_value_string_utf8(env, args[3], path, sizeof(path), nullptr);
    napi_get_value_int32(env, args[4], &policy);
    napi_get_value_string_utf8(env, args[5], headers, sizeof(headers), nullptr);
    napi_get_value_string_utf8(env, args[6], body, sizeof(body), nullptr);
    napi_get_value_int32(env, args[7], &timeoutMs);
    napi_get_value_bool(env, args[8], &idempotent);

    // 构造请求
    asternet_request_t req{};
    req.host = host;
    req.port = static_cast<uint16_t>(port);
    req.method = method;
    req.path = path;
    req.protocol_policy = static_cast<asternet_protocol_policy_t>(policy);
    req.timeout_ms = timeoutMs;
    req.idempotent = idempotent ? 1 : 0;

    // 解析 headers
    std::vector<asternet_header_t> hdrVec;
    std::vector<std::string> hdrStorage;
    if (headers[0]) {
        std::string raw(headers);
        size_t pos = 0;
        while (pos < raw.size()) {
            size_t end = raw.find('\n', pos);
            if (end == std::string::npos) end = raw.size();
            std::string line = raw.substr(pos, end - pos);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                hdrStorage.push_back(line.substr(0, colon));
                hdrStorage.push_back(line.substr(colon + 1));
                while (!hdrStorage.back().empty() && hdrStorage.back()[0] == ' ')
                    hdrStorage.back().erase(0, 1);
                hdrVec.push_back({hdrStorage[hdrStorage.size()-2].c_str(),
                                  hdrStorage.back().c_str()});
            }
            pos = end + 1;
        }
    }
    req.headers = hdrVec.empty() ? nullptr : hdrVec.data();
    req.header_count = hdrVec.size();

    std::string bodyStr(body);
    req.body = bodyStr.empty() ? nullptr : reinterpret_cast<const uint8_t*>(bodyStr.data());
    req.body_len = bodyStr.size();

    // 执行请求
    std::vector<uint8_t> respBuf(4 * 1024 * 1024);
    asternet_response_info_t respInfo{};
    asternet_result_t ret = asternet_client_request_sync(
        g_client, &req, respBuf.data(), respBuf.size(), &respInfo);

    // 构造 JSON 响应
    std::string json = "{";
    json += "\"result\":" + std::to_string(ret);
    json += ",\"error\":\"" + std::string(asternet_result_string(ret)) + "\"";
    json += ",\"status\":" + std::to_string(respInfo.http_status);
    json += ",\"protocol\":" + std::to_string(respInfo.protocol);
    json += ",\"degraded\":" + std::to_string(respInfo.degraded);
    json += ",\"bodySize\":" + std::to_string(respInfo.body_size);
    json += ",\"dnsMs\":" + std::to_string(respInfo.dns_ms);
    json += ",\"connectMs\":" + std::to_string(respInfo.connect_ms);
    json += ",\"tlsMs\":" + std::to_string(respInfo.tls_ms);
    json += ",\"ttfbMs\":" + std::to_string(respInfo.ttfb_ms);
    json += ",\"totalMs\":" + std::to_string(respInfo.total_ms);
    json += ",\"body\":\"";
    if (ret == ASTERNET_OK && respInfo.body_copied > 0) {
        for (size_t i = 0; i < respInfo.body_copied && i < 4000; i++) {
            char ch = static_cast<char>(respBuf[i]);
            if (ch == '"') json += "\\\"";
            else if (ch == '\\') json += "\\\\";
            else if (ch == '\n') json += "\\n";
            else if (ch == '\r') json += "\\r";
            else if (ch == '\t') json += "\\t";
            else if (ch >= 0x20) json += ch;
        }
    }
    json += "\"}";

    napi_value napiResult;
    napi_create_string_utf8(env, json.c_str(), json.size(), &napiResult);
    return napiResult;
}

// ---- NAPI: asternetVersion / asternetDestroy ----
static napi_value NapiVersion(napi_env env, napi_callback_info /*info*/) {
    napi_value ret;
    napi_create_string_utf8(env, asternet_version(), NAPI_AUTO_LENGTH, &ret);
    return ret;
}

static napi_value NapiDestroy(napi_env env, napi_callback_info /*info*/) {
    if (g_client) { asternet_client_destroy(g_client); g_client = nullptr; }
    napi_value ret;
    napi_get_undefined(env, &ret);
    return ret;
}

// ---- 模块注册 ----
EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"createClient", nullptr, NapiCreateClient, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"request",      nullptr, NapiRequest,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"version",      nullptr, NapiVersion,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroy",      nullptr, NapiDestroy,      nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module asternetModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "asternet_napi",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterAsternetModule(void) {
    napi_module_register(&asternetModule);
}

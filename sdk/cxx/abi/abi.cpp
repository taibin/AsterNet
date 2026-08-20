/* AsterNet public C ABI implementation. */
#include "asternet/asternet.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <memory>
#include <mutex>
#include <new>
#include <limits>
#include <string>
#include <unordered_map>

#include "client.h"
#include "export.h"
#include "asternet/version.h"
#include "platform/log.h"

namespace {

struct ClientHandle {
    std::mutex mutex;
    std::shared_ptr<asternet::Client> client;
    size_t active_requests = 0;
    bool destroying = false;
};

std::mutex g_handles_mutex;
std::unordered_map<asternet_client_t *, std::shared_ptr<ClientHandle>> g_handles;
uintptr_t g_next_handle_id = 1;

std::shared_ptr<asternet::Client> acquire_client(
    asternet_client_t *opaque, std::shared_ptr<ClientHandle> &handle) {
    if (opaque == nullptr) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_handles_mutex);
        auto it = g_handles.find(opaque);
        if (it == g_handles.end()) return nullptr;
        handle = it->second;
    }
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->destroying || handle->client == nullptr) return nullptr;
    ++handle->active_requests;
    return handle->client;
}

void release_client(const std::shared_ptr<ClientHandle> &handle) {
    if (handle == nullptr) return;
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->active_requests > 0) --handle->active_requests;
    if (handle->destroying && handle->active_requests == 0) handle->client.reset();
}

struct ClientLease {
    std::shared_ptr<ClientHandle> handle;
    ~ClientLease() { release_client(handle); }
};

int default_timeout_ms(const asternet::Client &client, int request_timeout_ms) {
    if (request_timeout_ms > 0) return request_timeout_ms;
    if (client.config().default_timeout_ms > 0) return client.config().default_timeout_ms;
    return 15000;
}

bool is_idempotent_method(const char *method) {
    return std::strcmp(method, "GET") == 0 || std::strcmp(method, "HEAD") == 0
        || std::strcmp(method, "OPTIONS") == 0;
}

bool has_idempotency_key(const asternet_header_t *headers, size_t header_count) {
    for (size_t i = 0; i < header_count; ++i) {
        const char *name = headers[i].name;
        if (name == nullptr || headers[i].value == nullptr || headers[i].value[0] == '\0') continue;
        static constexpr char kKey[] = "idempotency-key";
        size_t length = std::strlen(name);
        if (length != sizeof(kKey) - 1) continue;
        bool match = true;
        for (size_t j = 0; j < length; ++j) {
            if (std::tolower(static_cast<unsigned char>(name[j])) != kKey[j]) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(headers[i].value);
             *p != '\0'; ++p) {
            if (!std::isspace(*p)) return true;
        }
    }
    return false;
}

bool contains_crlf(const char *value) {
    return value != nullptr && std::strpbrk(value, "\r\n") != nullptr;
}

bool valid_token(const char *value) {
    if (value == nullptr || value[0] == '\0') return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p != '\0'; ++p) {
        if (std::isalnum(*p) || std::strchr("!#$%&'*+-.^_`|~", *p) != nullptr) continue;
        return false;
    }
    return true;
}

bool valid_path(const char *value) {
    if (value == nullptr || value[0] != '/') return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p != '\0'; ++p) {
        if (*p <= 0x20 || *p == 0x7f) return false;
    }
    return true;
}

bool valid_host(const char *value) {
    if (value == nullptr || value[0] == '\0') return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p != '\0'; ++p) {
        if (*p <= 0x20 || *p == 0x7f) return false;
    }
    return true;
}

bool valid_header_name(const char *name) {
    if (name == nullptr || name[0] == '\0') return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p != '\0'; ++p) {
        if (std::isalnum(*p) || std::strchr("!#$%&'*+-.^_`|~", *p) != nullptr) continue;
        return false;
    }
    return true;
}

bool protected_header(const char *name) {
    std::string lower(name);
    for (char &ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return lower == "host" || lower == "content-length" || lower == "transfer-encoding"
        || lower == "connection";
}

void reset_response_info(asternet_response_info_t *info) {
    if (info == nullptr) return;
    *info = {};
    info->result = ASTERNET_ERR_INTERNAL;
    info->protocol = ASTERNET_PROTOCOL_UNKNOWN;
    info->dns_ms = -1;
    info->connect_ms = -1;
    info->tls_ms = -1;
    info->ttfb_ms = -1;
    info->total_ms = -1;
}

void fill_response_info(const asternet::engine::Response &response, asternet_result_t result,
                        asternet_response_info_t *info) {
    if (info == nullptr) return;
    info->result = result;
    info->http_status = response.http_status;
    info->protocol = response.protocol;
    info->degraded = response.degraded ? 1 : 0;
    info->body_size = response.body.size();
    info->dns_ms = response.dns_ms;
    info->connect_ms = response.connect_ms;
    info->tls_ms = response.tls_ms;
    info->ttfb_ms = response.ttfb_ms;
    info->total_ms = response.total_ms;
}

}  // namespace

extern "C" {

ASTERNET_API const char *asternet_version(void) {
    return ASTERNET_VERSION_STRING;
}

ASTERNET_API const char *asternet_result_string(asternet_result_t result) {
    switch (result) {
    case ASTERNET_OK: return "OK";
    case ASTERNET_ERR_ABI_VERSION: return "ABI_VERSION";
    case ASTERNET_ERR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case ASTERNET_ERR_NOT_INITIALIZED: return "NOT_INITIALIZED";
    case ASTERNET_ERR_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case ASTERNET_ERR_TIMEOUT: return "TIMEOUT";
    case ASTERNET_ERR_DNS: return "DNS";
    case ASTERNET_ERR_CONNECT: return "CONNECT";
    case ASTERNET_ERR_TLS: return "TLS";
    case ASTERNET_ERR_PROTOCOL: return "PROTOCOL";
    case ASTERNET_ERR_CANCELED: return "CANCELED";
    case ASTERNET_ERR_DEGRADED: return "DEGRADED";
    case ASTERNET_ERR_NETWORK_CHANGED: return "NETWORK_CHANGED";
    case ASTERNET_ERR_UNSUPPORTED: return "UNSUPPORTED";
    case ASTERNET_ERR_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case ASTERNET_ERR_INTERNAL: return "INTERNAL";
    default: return "UNKNOWN";
    }
}

ASTERNET_API void asternet_set_log_callback(asternet_log_callback_t callback, void *user_data,
                                             int level) {
    try {
        asternet::platform::set_log_callback(callback, user_data, level);
    } catch (...) {
        // C ABI 无法向调用方传播 C++ 异常，日志注册失败时保持旧回调。
    }
}

ASTERNET_API asternet_client_t *asternet_client_create(const asternet_client_config_t *config,
                                                        asternet_result_t *out_error) {
    if (out_error != nullptr) *out_error = ASTERNET_OK;
    if (config == nullptr) {
        if (out_error != nullptr) *out_error = ASTERNET_ERR_INVALID_ARGUMENT;
        return nullptr;
    }
    const size_t minimum_config_size = offsetof(asternet_client_config_t, ca_cert_pem)
                                     + sizeof(config->ca_cert_pem);
    if (config->struct_size < minimum_config_size) {
        if (out_error != nullptr) *out_error = ASTERNET_ERR_ABI_VERSION;
        return nullptr;
    }
    asternet_client_config_t normalized_config{};
    const size_t copy_size = std::min(config->struct_size, sizeof(normalized_config));
    std::memcpy(&normalized_config, config, copy_size);
    normalized_config.struct_size = sizeof(normalized_config);
    if (!asternet::Client::check_abi(normalized_config.abi_version)) {
        if (out_error != nullptr) *out_error = ASTERNET_ERR_ABI_VERSION;
        return nullptr;
    }

    try {
        auto owned_client = std::make_shared<asternet::Client>(normalized_config);
        auto handle = std::make_shared<ClientHandle>();
        handle->client = std::move(owned_client);
        std::lock_guard<std::mutex> lock(g_handles_mutex);
        if (g_next_handle_id > (std::numeric_limits<uintptr_t>::max() >> 1)) {
            if (out_error != nullptr) *out_error = ASTERNET_ERR_OUT_OF_MEMORY;
            return nullptr;
        }
        const uintptr_t token = (g_next_handle_id++ << 1) | 1;
        auto *opaque = reinterpret_cast<asternet_client_t *>(token);
        g_handles.emplace(opaque, std::move(handle));
        return opaque;
    } catch (...) {
        if (out_error != nullptr) *out_error = ASTERNET_ERR_OUT_OF_MEMORY;
        return nullptr;
    }
}

ASTERNET_API void asternet_client_destroy(asternet_client_t *client) {
    try {
        std::shared_ptr<ClientHandle> handle;
        {
            std::lock_guard<std::mutex> lock(g_handles_mutex);
            auto it = g_handles.find(client);
            if (it == g_handles.end()) return;
            handle = std::move(it->second);
            g_handles.erase(it);
        }
        std::lock_guard<std::mutex> lock(handle->mutex);
        handle->destroying = true;
        if (handle->active_requests == 0) handle->client.reset();
    } catch (...) {
        // 句柄已从 registry 移除时保持失效，避免异常越过 C ABI。
    }
}

ASTERNET_API void asternet_client_on_network_change(asternet_client_t *client,
                                                      asternet_network_t network) {
    try {
        std::shared_ptr<ClientHandle> handle;
        auto owned_client = acquire_client(client, handle);
        if (owned_client == nullptr) return;
        ClientLease lease{handle};
        owned_client->on_network_change(network);
    } catch (...) {
        // 网络切换通知是 best-effort，不允许异常越过 C ABI。
    }
}

ASTERNET_API asternet_result_t asternet_client_prefetch(asternet_client_t *client,
                                                        const char *host) {
    if (client == nullptr || !valid_host(host)) {
        return ASTERNET_ERR_INVALID_ARGUMENT;
    }
    try {
        std::shared_ptr<ClientHandle> handle;
        auto owned_client = acquire_client(client, handle);
        if (owned_client == nullptr) return ASTERNET_ERR_INVALID_ARGUMENT;
        ClientLease lease{handle};
        return static_cast<asternet_result_t>(owned_client->prefetch(host));
    } catch (const std::bad_alloc &) {
        return ASTERNET_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return ASTERNET_ERR_INTERNAL;
    }
}

ASTERNET_API void asternet_client_set_quality_callback(asternet_client_t *client,
                                                       asternet_quality_callback_t callback,
                                                       void *user_data) {
    try {
        std::shared_ptr<ClientHandle> handle;
        auto owned_client = acquire_client(client, handle);
        if (owned_client == nullptr) return;
        ClientLease lease{handle};
        owned_client->set_quality_change_callback(callback, user_data);
    } catch (...) {
        // 质量回调注册失败时保持旧状态，不允许异常越过 C ABI。
    }
}

ASTERNET_API asternet_result_t asternet_client_request_sync(
    asternet_client_t *client, const asternet_request_t *request, uint8_t *out_body,
    size_t out_body_capacity, asternet_response_info_t *out_info) {
    reset_response_info(out_info);
    if (client == nullptr || request == nullptr || request->host == nullptr || request->method == nullptr
        || request->path == nullptr || (request->body_len > 0 && request->body == nullptr)
        || (request->header_count > 0 && request->headers == nullptr)
        || !valid_host(request->host) || !valid_token(request->method) || !valid_path(request->path)) {
        if (out_info != nullptr) out_info->result = ASTERNET_ERR_INVALID_ARGUMENT;
        return ASTERNET_ERR_INVALID_ARGUMENT;
    }
    if (request->port == 0 || request->timeout_ms < 0) {
        if (out_info != nullptr) out_info->result = ASTERNET_ERR_INVALID_ARGUMENT;
        return ASTERNET_ERR_INVALID_ARGUMENT;
    }

    try {
    std::shared_ptr<ClientHandle> handle;
    auto internal_client = acquire_client(client, handle);
    if (internal_client == nullptr) {
        if (out_info != nullptr) out_info->result = ASTERNET_ERR_INVALID_ARGUMENT;
        return ASTERNET_ERR_INVALID_ARGUMENT;
    }
    ClientLease lease{handle};
    asternet::engine::Request internal_request;
    internal_request.host = request->host;
    internal_request.port = request->port;
    internal_request.method = request->method;
    internal_request.path = request->path;
    internal_request.timeout_ms = default_timeout_ms(*internal_client,
                                                       request->timeout_ms);
    // Kept in the ABI for backward layout compatibility. TLS verification bypasses are never
    // enabled by the production core.
    internal_request.allow_insecure_tls_for_testing = false;
    internal_request.ca_cert_pem = internal_client->ca_cert_pem();
    const int configured_max_body = internal_client->config().max_response_body_bytes;
    if (configured_max_body > 0) {
        internal_request.max_response_body_bytes = static_cast<size_t>(configured_max_body);
    }
    if (request->body_len > 0) {
        internal_request.body.assign(reinterpret_cast<const char *>(request->body), request->body_len);
    }
    internal_request.idempotent = request->idempotent != 0
        || (internal_request.body.empty() && is_idempotent_method(request->method));
    internal_request.retry_safe = (internal_request.body.empty() && is_idempotent_method(request->method))
        || (request->idempotent != 0 && has_idempotency_key(request->headers, request->header_count));
    internal_request.headers.reserve(request->header_count);
    for (size_t i = 0; i < request->header_count; ++i) {
        const asternet_header_t &header = request->headers[i];
        if (!valid_header_name(header.name) || header.value == nullptr
            || contains_crlf(header.name) || contains_crlf(header.value)
            || protected_header(header.name)) {
            if (out_info != nullptr) out_info->result = ASTERNET_ERR_INVALID_ARGUMENT;
            return ASTERNET_ERR_INVALID_ARGUMENT;
        }
        internal_request.headers.push_back({header.name, header.value});
    }

    asternet::engine::Response response;
    asternet_protocol_t actual_protocol = ASTERNET_PROTOCOL_UNKNOWN;
    bool degraded = false;
    const int request_result = internal_client->request_with_policy(
        internal_request, request->protocol_policy, response, &actual_protocol, &degraded);
    // 仅在成功时覆盖协议（失败时保留引擎设置的协议，便于日志/调试）
    if (request_result == ASTERNET_OK) {
        response.protocol = actual_protocol;
    }
    response.degraded = degraded;
    const asternet_result_t result = static_cast<asternet_result_t>(request_result);
    fill_response_info(response, result, out_info);

    if (result != ASTERNET_OK) return result;
    if ((!response.body.empty() && out_body == nullptr)
        || out_body_capacity < response.body.size()) {
        if (out_info != nullptr) out_info->result = ASTERNET_ERR_BUFFER_TOO_SMALL;
        return ASTERNET_ERR_BUFFER_TOO_SMALL;
    }
    if (!response.body.empty()) {
        std::memcpy(out_body, response.body.data(), response.body.size());
    }
    if (out_info != nullptr) out_info->body_copied = response.body.size();
    return ASTERNET_OK;
    } catch (const std::bad_alloc &) {
        if (out_info != nullptr) out_info->result = ASTERNET_ERR_OUT_OF_MEMORY;
        return ASTERNET_ERR_OUT_OF_MEMORY;
    } catch (...) {
        if (out_info != nullptr) out_info->result = ASTERNET_ERR_INTERNAL;
        return ASTERNET_ERR_INTERNAL;
    }
}

ASTERNET_API size_t asternet_client_dump_diagnostics(asternet_client_t *client, char *out_buffer,
                                                      size_t out_capacity) {
    if (out_buffer == nullptr || out_capacity == 0) return 0;
    std::string diagnostics;
    try {
        std::shared_ptr<ClientHandle> handle;
        auto owned_client = acquire_client(client, handle);
        if (owned_client == nullptr) return 0;
        ClientLease lease{handle};
        diagnostics = owned_client->dump_diag();
    } catch (...) {
        diagnostics = "{\"error\":\"OUT_OF_MEMORY\"}";
    }
    const size_t copied = diagnostics.size() < out_capacity - 1
        ? diagnostics.size()
        : out_capacity - 1;
    std::memcpy(out_buffer, diagnostics.data(), copied);
    out_buffer[copied] = '\0';
    return copied;
}

ASTERNET_API size_t asternet_client_trace_route(asternet_client_t *client, const char *host,
                                                uint16_t port, char *out_buffer,
                                                size_t out_capacity) {
    if (out_buffer == nullptr || out_capacity == 0 || !valid_host(host) || port == 0) return 0;
    std::string diagnostics;
    try {
        std::shared_ptr<ClientHandle> handle;
        auto owned_client = acquire_client(client, handle);
        if (owned_client == nullptr) return 0;
        ClientLease lease{handle};
        diagnostics = owned_client->trace_route(host, port);
    } catch (...) {
        diagnostics = "{\"error\":\"OUT_OF_MEMORY\"}";
    }
    const size_t copied = diagnostics.size() < out_capacity - 1
        ? diagnostics.size()
        : out_capacity - 1;
    std::memcpy(out_buffer, diagnostics.data(), copied);
    out_buffer[copied] = '\0';
    return copied;
}

}  // extern "C"

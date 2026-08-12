/*
 * AsterNet public C ABI.
 *
 * This header is C-compatible. Applications must not pass STL types or retain
 * pointers owned by AsterNet across an API call.
 */
#ifndef ASTERNET_PUBLIC_API_H
#define ASTERNET_PUBLIC_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASTERNET_ABI_VERSION_MAJOR 1
#define ASTERNET_ABI_VERSION_MINOR 1
#define ASTERNET_ABI_VERSION \
    ((ASTERNET_ABI_VERSION_MAJOR << 16) | ASTERNET_ABI_VERSION_MINOR)

typedef struct asternet_client asternet_client_t;

typedef enum {
    ASTERNET_OK = 0,
    ASTERNET_ERR_ABI_VERSION = -1,
    ASTERNET_ERR_INVALID_ARGUMENT = -2,
    ASTERNET_ERR_NOT_INITIALIZED = -3,
    ASTERNET_ERR_OUT_OF_MEMORY = -4,
    ASTERNET_ERR_TIMEOUT = -5,
    ASTERNET_ERR_DNS = -6,
    ASTERNET_ERR_CONNECT = -7,
    ASTERNET_ERR_TLS = -8,
    ASTERNET_ERR_PROTOCOL = -9,
    ASTERNET_ERR_CANCELED = -10,
    ASTERNET_ERR_DEGRADED = -11,
    ASTERNET_ERR_NETWORK_CHANGED = -12,
    ASTERNET_ERR_UNSUPPORTED = -13,
    ASTERNET_ERR_BUFFER_TOO_SMALL = -14,
    ASTERNET_ERR_INTERNAL = -100,
} asternet_result_t;

typedef enum {
    ASTERNET_PROTOCOL_UNKNOWN = 0,
    ASTERNET_PROTOCOL_HTTP_1_1 = 1,
    ASTERNET_PROTOCOL_HTTP_2 = 2,
    ASTERNET_PROTOCOL_HTTP_3 = 3,
} asternet_protocol_t;

typedef enum {
    ASTERNET_POLICY_AUTO = 0,
    ASTERNET_POLICY_HTTP_1_1_ONLY = 1,
    ASTERNET_POLICY_HTTP_2_ONLY = 2,
    ASTERNET_POLICY_HTTP_3_ONLY = 3,
    ASTERNET_POLICY_PREFER_HTTP_3 = 4,
    ASTERNET_POLICY_PREFER_HTTP_2 = 5,
} asternet_protocol_policy_t;

typedef enum {
    ASTERNET_NETWORK_UNKNOWN = 0,
    ASTERNET_NETWORK_NONE = 1,
    ASTERNET_NETWORK_WIFI = 2,
    ASTERNET_NETWORK_CELLULAR = 3,
    ASTERNET_NETWORK_ETHERNET = 4,
} asternet_network_t;

typedef struct {
    const char *name;
    const char *value;
} asternet_header_t;

typedef struct {
    size_t struct_size;
    uint32_t abi_version;
    int default_timeout_ms;
    int max_response_body_bytes;
    int enable_http3;
    int allow_insecure_tls_for_testing;
    const char *ca_cert_pem;
    // 默认 0：拒绝 RFC1918、loopback、link-local 和 multicast 地址，避免远端配置
    // 或不可信输入被用作 SSRF 跳板。仅可信内网部署可显式设为 1。
    int allow_private_networks;
} asternet_client_config_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *method;
    const char *path;
    const asternet_header_t *headers;
    size_t header_count;
    const uint8_t *body;
    size_t body_len;
    asternet_protocol_policy_t protocol_policy;
    int timeout_ms;
    int idempotent;
    int allow_insecure_tls_for_testing;
} asternet_request_t;

typedef struct {
    asternet_result_t result;
    int http_status;
    asternet_protocol_t protocol;
    int degraded;
    size_t body_size;
    size_t body_copied;
    int64_t dns_ms;
    int64_t connect_ms;
    int64_t tls_ms;
    int64_t ttfb_ms;
    int64_t total_ms;
} asternet_response_info_t;

typedef void (*asternet_log_callback_t)(int level, const char *tag, const char *message,
                                        void *user_data);

asternet_client_t *asternet_client_create(const asternet_client_config_t *config,
                                           asternet_result_t *out_error);
void asternet_client_destroy(asternet_client_t *client);
void asternet_client_on_network_change(asternet_client_t *client,
                                       asternet_network_t network);

/*
 * Executes a request synchronously.
 *
 * AsterNet always fills out_info when it is non-null. When out_body is null or
 * too small, body_size reports the required byte count and the function returns
 * ASTERNET_ERR_BUFFER_TOO_SMALL after the network request has completed.
 */
asternet_result_t asternet_client_request_sync(
    asternet_client_t *client, const asternet_request_t *request, uint8_t *out_body,
    size_t out_body_capacity, asternet_response_info_t *out_info);

size_t asternet_client_dump_diagnostics(asternet_client_t *client, char *out_buffer,
                                        size_t out_capacity);
void asternet_set_log_callback(asternet_log_callback_t callback, void *user_data, int level);
const char *asternet_version(void);
const char *asternet_result_string(asternet_result_t result);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ASTERNET_PUBLIC_API_H */

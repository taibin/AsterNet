# AsterNet API Reference

## Overview

AsterNet exposes a **C-compatible ABI** (`asternet/asternet.h`) that is callable from any language with C FFI support: Java (JNI), Kotlin, Swift, Objective-C, ArkTS (NAPI), Python (ctypes), Rust (bindgen).

## C ABI

### Types

```c
typedef struct asternet_client asternet_client_t;  // Opaque client handle

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
    ASTERNET_POLICY_AUTO = 0,           // H3 → H2 → H1.1 fallback
    ASTERNET_POLICY_HTTP_1_1_ONLY = 1,  // Force HTTP/1.1, no fallback
    ASTERNET_POLICY_HTTP_2_ONLY = 2,    // Force HTTP/2, no fallback
    ASTERNET_POLICY_HTTP_3_ONLY = 3,    // Force HTTP/3, no fallback
    ASTERNET_POLICY_PREFER_HTTP_3 = 4,  // H3 first, allow fallback
    ASTERNET_POLICY_PREFER_HTTP_2 = 5,  // H2 first, allow fallback
} asternet_protocol_policy_t;
```

### Client Lifecycle

```c
// Configuration (all fields must be set, struct_size for ABI versioning)
typedef struct {
    size_t struct_size;              // sizeof(asternet_client_config_t)
    uint32_t abi_version;            // ASTERNET_ABI_VERSION
    int default_timeout_ms;          // Default 12000
    int max_response_body_bytes;     // Default 4 MB
    int enable_http3;                // 0 or 1
    int allow_insecure_tls_for_testing; // 0 or 1 (NEVER enable in production)
    const char *ca_cert_pem;         // NULL = system defaults
} asternet_client_config_t;

// Create/destroy
asternet_client_t *asternet_client_create(const asternet_client_config_t *config,
                                           asternet_result_t *out_error);
void asternet_client_destroy(asternet_client_t *client);
```

### Synchronous Request

```c
typedef struct {
    const char *host;                      // Required
    uint16_t port;                         // Typically 443
    const char *method;                    // "GET", "POST", etc.
    const char *path;                      // "/" or "/api/resource"
    const asternet_header_t *headers;      // NULL or array
    size_t header_count;
    const uint8_t *body;                   // NULL for GET
    size_t body_len;
    asternet_protocol_policy_t protocol_policy;
    int timeout_ms;                        // 0 = use client default
    int idempotent;                        // 1 = safe to retry/fallback
} asternet_request_t;

typedef struct {
    int64_t dns_ms;       // -1 if not measured
    int64_t connect_ms;   // -1 if not measured
    int64_t tls_ms;       // -1 if not measured
    int64_t ttfb_ms;      // -1 if not measured
    int64_t total_ms;     // -1 if not measured
} asternet_response_info_t;

asternet_result_t asternet_client_request_sync(
    asternet_client_t *client,
    const asternet_request_t *request,
    uint8_t *out_body,                // Pre-allocated buffer
    size_t out_body_capacity,         // Buffer size
    asternet_response_info_t *out_info // NULL or pointer
);
```

**Usage:**

```c
uint8_t buf[4 * 1024 * 1024];  // 4 MB response buffer
asternet_response_info_t info;
asternet_request_t req = {
    .host = "api.example.com",
    .port = 443,
    .method = "POST",
    .path = "/v1/data",
    .body = (uint8_t*)"{\"key\":\"value\"}",
    .body_len = 16,
    .protocol_policy = ASTERNET_POLICY_AUTO,
    .timeout_ms = 5000,
    .idempotent = 0  // POST = non-idempotent, no fallback on failure
};

asternet_result_t r = asternet_client_request_sync(client, &req, buf, sizeof(buf), &info);
if (r == ASTERNET_OK) {
    printf("HTTP %d, %zu bytes, %lld ms\n", info.http_status, info.body_size, info.total_ms);
}
```

### Utility

```c
const char *asternet_version(void);
const char *asternet_result_string(asternet_result_t result);

// Forward native logs to application logger
void asternet_set_log_callback(asternet_log_callback_t callback, void *user_data, int level);
```

## Android (Java)

```java
// Create
AsterNet.Client client = AsterNet.createClient(
    true,           // enableH3
    caBundlePem     // CA certificates or null
);

// Request
AsterNet.Response resp = client.request(
    "host", 443, "GET", "/path",
    AsterNet.Policy.AUTO,   // protocol policy
    "",                     // headers ("Name: Value\n")
    new byte[0],            // body
    12000,                  // timeout ms
    true                    // idempotent
);

// Response fields
resp.result          // 0 = OK, negative = error code
resp.error           // Error name string
resp.status          // HTTP status code
resp.protocolName()  // "HTTP/1.1", "HTTP/2", "HTTP/3"
resp.body            // Response body string
resp.dnsMs           // DNS resolution time
resp.connectMs       // TCP connect time
resp.tlsMs           // TLS handshake time
resp.ttfbMs          // Time to first byte
resp.totalMs         // Core request time

// Cleanup
client.close();
```

## iOS (Swift)

```swift
let client = AsterNetClient()
let resp = await client.request(
    host: "api.example.com", port: 443,
    method: "GET", path: "/",
    policy: .auto,
    headers: [:], body: Data(),
    timeoutMs: 12000, idempotent: true
)
print("HTTP \(resp.httpStatus) via \(resp.proto.label)")
```

## Error Handling

| Code | Constant | Meaning | Recovery |
|------|----------|---------|----------|
| 0 | OK | Success | — |
| -5 | TIMEOUT | Request exceeded timeout_ms | Retry with longer timeout |
| -6 | DNS | Name resolution failed | Check hostname, network |
| -7 | CONNECT | TCP connection refused/timeout | Check server, firewall, network |
| -8 | TLS | Certificate verification failed | Check CA bundle, server cert |
| -9 | PROTOCOL | HTTP framing error | Check server compatibility |
| -13 | UNSUPPORTED | Requested protocol not available | Use AUTO policy |
| -14 | BUFFER_TOO_SMALL | Response body exceeds buffer | Increase max_response_body_bytes |

## Thread Safety

- `asternet_client_create` / `asternet_client_destroy`: **Not thread-safe** (call from one thread)
- `asternet_client_request_sync`: **Thread-safe** (internal mutex, serializes requests per client)
- Multiple clients: Create separate `asternet_client_t*` instances for independent request streams

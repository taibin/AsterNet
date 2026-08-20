# AsterNet

**Cross-platform C++ networking core with HTTP/1.1, HTTP/2, and HTTP/3 via QUIC.**

Built for mobile SDK teams who need direct protocol control, multi-platform consistency, and independence from platform HTTP stacks.

[中文文档](README.zh.md)

## Why AsterNet

Platform HTTP clients (OkHttp, URLSession, `@ohos.net.http`) are black boxes — you cannot inspect protocol negotiation, measure per-phase latency, or customize TLS behavior. AsterNet gives you full control: DNS → TCP/QUIC → TLS → HTTP frames, every byte accounted for.

| | OkHttp | Cronet | AsterNet |
|---|--------|--------|----------|
| **Cross-platform** | Android/JVM only | Android (Chromium) | Android, iOS, HarmonyOS, Linux/macOS |
| **Protocol control** | Limited (ALPN hints) | Limited | Per-request: AUTO / H1 / H2 / H3 |
| **HTTP/3** | Experimental | ✅ | ✅ (XQUIC) |
| **Per-phase metrics** | ❌ | Limited | ✅ DNS, Connect, TLS, TTFB, Total |
| **TLS customization** | HostnameVerifier only | ❌ | ✅ Custom CA bundle, cert verify callback |
| **Binary size** | ~1 MB (JVM) | ~8 MB (native) | ~3 MB (native, without H3) |
| **Concurrency model** | Thread pool | Thread pool | Synchronous API, caller controls threading |
| **Memory** | GC-managed | Native heap | Native heap, no GC pressure |

## Architecture

<img src="docs/images/mermaid-diagram-2026-08-08-194816.png" alt="Asternet Architecture" width=585.9 height=809.1/>


## Features

### Protocol Stack
- **HTTP/1.1** — self-built engine with chunked encoding, keep-alive
- **HTTP/2** — full nghttp2 integration, multiplexing, HPACK, server push
- **HTTP/3** — QUIC via alibaba/xquic, 0-RTT, connection migration

### Protocol Selection
- **AUTO** — H3 → H2 → H1.1 automatic fallback with circuit breaker
- **Per-request control** — force any protocol, skip automatic downgrade
- **Host-level circuit breaker** — 3 consecutive failures = 60s cooldown per host

### Smart DNS Resolution
- **HTTPDNS → LocalDNS → Backup IP** three-tier fallback chain
- **Health-based IP scoring** — RTT / loss / failure penalties, stable sort (best IP first)
- **TTL cache** with stale-while-revalidate + LRU eviction, keyed by `network_epoch`
- IP literal fast path + `prefetch` warm-up

### Weak Network Detection & Optimization
- **Quality scoring** — RTT / loss / bandwidth weighted, `UNKNOWN → HEALTHY → DEGRADED → BAD` state machine
- **EMA-smoothed RTT** + passive observation via QUIC connection stats (zero probe overhead)
- **Adaptive policy** — timeout / concurrency / retry tuned by live network quality

### Connection Pool
- **Lease model** — `acquire → use → release`, explicit lifecycle
- **LRU eviction** + full cleanup on network change

### Request Orchestration
- **Interceptor chain** — weak-network guard + retry
- **Retry** — exponential backoff + jitter, shared deadline, idempotency auto-detection
- **Request coalescing** — GET/HEAD dedup, auth/proxy/CA isolated, header whitelist

### Security
- TLS 1.3 with BoringSSL (same engine as Chromium)
- Custom CA certificate bundle injection
- Certificate verification callback (inspect chain before trust)
- DNS SSRF protection — private/special IP filtering (IPv4 + 6 IPv6-embedded-IPv4 formats)
- `allow_insecure_tls_for_testing` for local development

### Observability
- Per-phase latency: DNS, Connect, TLS, TTFB, Total (core + wall clock)
- Protocol used (even on fallback) + fallback flag
- Native log callback → integrate with any logging system
- Diagnostics dump (connection pool, DNS cache, quality snapshot)

### Network Diagnostics
- **Trace route** — per-hop TTL + per-probe RTT, timeout shown as `*` (no root required)
- **Network change notification** — resets quality probe + connection pool on switch

### Platform Support
| Platform | Status | SDK Format |
|----------|--------|------------|
| Android | ✅ Production | AAR + .so (arm64-v8a, 16KB page) |
| iOS | 🚧 Example ready | xcframework (pending) |
| HarmonyOS | 🚧 Example ready | .har (pending) |
| Linux/macOS | ✅ C++ tests | static/shared library |

## Quick Start

### Android

```bash
# 1. Setup third-party dependencies (one command)
./sdk/third_party/repack.sh

# 2. Build APK
./gradlew :examples:android:assembleDebug

# 3. Install
adb install -r examples/android/build/outputs/apk/debug/android-debug.apk
```

The demo has three tabs:

| Tab | What it shows |
|-----|---------------|
| **Presets** | One-tap scenarios (`Automatic GET`, `HTTP/1.1/2/3 GET`, `HTTP/2 POST`) with full per-phase timeline |
| **Custom** | Build any request — method, protocol, headers, timeout, insecure TLS |
| **Diagnostics** | Live network state, quality snapshot, trace route, diagnostics dump |

<p align="left">
  <img src="docs/images/Screenshot_20260820_131956.png" alt="Presets" width="200"/>
  <img src="docs/images/Screenshot_20260820_132038.png" alt="Custom" width="200"/>
  <img src="docs/images/Screenshot_20260820_132325.png" alt="Diagnostics" width="200"/>
</p>

### C++ (Desktop)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## API

### C ABI (all platforms)

```c
#include "asternet/asternet.h"

// Create client
asternet_client_config_t cfg = { .struct_size = sizeof(cfg),
    .abi_version = ASTERNET_ABI_VERSION,
    .default_timeout_ms = 12000,
    .enable_http3 = 1 };
asternet_client_t *client = asternet_client_create(&cfg, NULL);

// Synchronous request
asternet_request_t req = { .host = "www.cloudflare.com", .port = 443,
    .method = "GET", .path = "/",
    .protocol_policy = ASTERNET_POLICY_AUTO,
    .timeout_ms = 12000 };
uint8_t body[1024 * 1024];
asternet_response_info_t info;
asternet_client_request_sync(client, &req, body, sizeof(body), &info);

printf("HTTP %d via %s, %zu bytes, %lld ms\n",
    info.http_status,
    info.protocol == ASTERNET_PROTOCOL_HTTP_3 ? "H3" : "H2/H1",
    info.body_size, info.total_ms);

asternet_client_destroy(client);
```

### Android (Java/Kotlin)

```java
AsterNet.Client client = AsterNet.createClient(true, caBundlePem);
AsterNet.Response resp = client.request(
    "www.cloudflare.com", 443, "GET", "/",
    AsterNet.Policy.AUTO, "", new byte[0], 12000, true);
System.out.println(resp.protocolName() + " " + resp.status);
client.close();
```

See [API Reference](docs/API.md) for full details.

## Performance

| Metric | Value | Notes |
|--------|-------|-------|
| HTTP/1.1 TTFB | ~50–200 ms | Depends on network RTT |
| HTTP/2 TTFB | ~40–150 ms | Multiplexed, single connection |
| HTTP/3 (QUIC) TTFB | ~10–30 ms | 0-RTT possible on reconnect |
| H3 connection setup | ~1 handshake | vs H2: TCP + TLS (2–3 round trips) |
| Binary size (all .so) | ~7.5 MB | 6 independent .so files |
| Memory per request | ~1 MB | Configurable buffer size |
| Concurrent requests | Unlimited | Caller controls thread pool |

## Concurrency & Stability

- **Synchronous API** — no hidden thread pools, no callback hell. Caller chooses threading model.
- **Thread-safe** — single client instance can be shared across threads (mutex per request).
- **Circuit breaker** — per-host, per-protocol. 3 failures → 60s cooldown. Prevents cascading failures.
- **No GC pressure** — C++ core, zero allocations on JVM heap during requests.
- **Deterministic resource lifecycle** — `create` → `request` → `destroy`. No leaked connections.

## Compared to OkHttp

See [docs/OKHTTP_COMPARISON.md](docs/OKHTTP_COMPARISON.md) for detailed analysis.

**Key differences:**

1. **Protocol visibility** — OkHttp hides protocol selection behind interceptors. AsterNet exposes `protocol_policy` per-request and reports actual protocol used.

2. **Latency breakdown** — OkHttp's `EventListener` gives callback-based timing. AsterNet returns `{dns_ms, connect_ms, tls_ms, ttfb_ms, total_ms}` in every response struct.

3. **HTTP/3** — OkHttp's H3 is experimental and limited to specific JVM builds. AsterNet's H3 is native QUIC via xquic, used in production by Alibaba Cloud CDN.

4. **Multi-platform** — OkHttp is JVM-only. AsterNet runs on iOS, HarmonyOS, and desktop via the same C ABI.

5. **Binary overhead** — OkHttp requires JVM + Kotlin stdlib (~2 MB). AsterNet is pure native code (~3 MB .so).

## Directory Structure

```text
sdk/                    SDK (deliverables)
├── cxx/                C++ core → libasternet.so
│   └── public/         Public C ABI headers
├── android/            Android JNI bridge + AAR
├── ios/                iOS Framework (placeholder)
├── harmonyos/          HarmonyOS NAPI bridge (placeholder)
└── third_party/        Third-party: nghttp2, xquic, BoringSSL → independent .so

examples/               Runnable examples
├── android/            Android Demo (Presets + Custom + Diagnostics tabs)
├── cxx/                C++ unit tests
├── ios/                iOS Demo (SwiftUI)
├── harmonyos/          HarmonyOS Demo (ArkTS)
└── server/             Go test server (H1/H2/H3)

scripts/                Build & release scripts
docs/                   Documentation
```

## License

Apache License 2.0. Third-party dependencies retain their original licenses.

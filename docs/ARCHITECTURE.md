# AsterNet Architecture

## System Overview

```mermaid
graph TB
    subgraph "SDK Layer"
        SDK_ANDROID["sdk/android<br/>AAR + libasternet-jni.so"]
        SDK_IOS["sdk/ios<br/>xcframework"]
        SDK_HOS["sdk/harmonyos<br/>har + libasternet_napi.so"]
    end

    subgraph "C ABI"
        ABI_H["asternet/asternet.h"]
    end

    subgraph "C++ Core"
        CLIENT["Client"]
        SELECTOR["ProtocolSelector"]
        H3["QuicEngine"]
        H2["Http2Engine"]
        H1["Http1Engine"]
        DNS["DNS Resolver"]
        TLS["TLS (BoringSSL)"]
    end

    subgraph "Third-party .so"
        XQUIC["libxquic.so"]
        NGHTTP2["libnghttp2.so"]
        BSSL["libssl.so + libcrypto.so"]
    end

    SDK_ANDROID --> ABI_H
    SDK_IOS --> ABI_H
    SDK_HOS --> ABI_H
    ABI_H --> CLIENT
    CLIENT --> SELECTOR
    SELECTOR --> H3
    SELECTOR --> H2
    SELECTOR --> H1
    H3 --> XQUIC
    H3 --> TLS
    H2 --> NGHTTP2
    H2 --> TLS
    H1 --> TLS
    TLS --> BSSL
    SELECTOR --> DNS
```

## Component Design

### ProtocolSelector — Fallback Chain

The core routing decision engine. Implements a priority-ordered fallback chain with per-host circuit breaking.

```mermaid
stateDiagram-v2
    [*] --> TryH3: AUTO / Prefer H3
    [*] --> TryH2: Prefer H2
    [*] --> TryH1: H1 Only

    TryH3 --> H3Success: OK
    TryH3 --> H3Fail: FAIL
    H3Fail --> TryH2: idempotent & retryable
    H3Fail --> AllFailed: non-idempotent or non-retryable

    TryH2 --> H2Success: OK
    TryH2 --> H2Fail: FAIL
    H2Fail --> TryH1: idempotent & retryable
    H2Fail --> AllFailed: non-idempotent or non-retryable

    TryH1 --> H1Success: OK
    TryH1 --> H1Fail: FAIL
    H1Fail --> AllFailed

    H3Success --> [*]
    H2Success --> [*]
    H1Success --> [*]
    AllFailed --> [*]
```

**Circuit breaker rules:**
- 3 consecutive failures for a protocol on a specific host → 60s ban
- Non-idempotent requests (POST, PATCH) → no automatic fallback
- Timeout consumed by failed attempts is deducted from remaining budget

### Engine Interface

```cpp
class NetworkEngine {
public:
    virtual int request(const Request &req, Response &resp) = 0;
    virtual EngineType type() const = 0;
    virtual EngineCaps caps() const = 0;
};
```

### Independent .so Architecture

```
APK lib/arm64-v8a/
├── libcrypto.so       (BoringSSL, 2.1 MB)
├── libssl.so          (BoringSSL, 551 KB)
├── libnghttp2.so      (HTTP/2, 240 KB)
├── libxquic.so        (QUIC/H3, 611 KB)
├── libasternet-jni.so (AsterNet JNI, 3.0 MB)
└── libc++_shared.so   (NDK runtime, 1.3 MB)
```

## Data Flow

```mermaid
sequenceDiagram
    participant App as Application
    participant JNI as JNI Bridge
    participant Client as Client
    participant Selector as ProtocolSelector
    participant Engine as NetworkEngine
    participant Net as Network

    App->>JNI: request(host, port, method, path)
    JNI->>Client: request_with_policy(req, policy)
    Client->>Selector: request_with_policy(req, policy)
    Selector->>Selector: Check circuit breaker
    Selector->>Engine: request(req, resp)
    Engine->>Net: DNS → connect → TLS → send → recv
    Net-->>Engine: HTTP response
    Engine-->>Selector: resp {status, body, timing}
    Selector-->>Client: resp
    Client-->>JNI: resp
    JNI-->>App: AsterNet.Response
```

## Security Architecture

### TLS Chain

```
Application
  └─ AndroidCAStore (Java) → PEM bundle (139 system certs)
      └─ asternet_client_config_t.ca_cert_pem
          └─ BoringSSL SSL_CTX → X509_STORE
              └─ SSL_connect → verify server cert chain
```

## Build Pipeline

```mermaid
graph LR
    subgraph "Sources"
        GH_XQUIC["GitHub: xquic v1.9.4"]
        GH_NG["GitHub: nghttp2 v1.70.0"]
        GH_BSSL["GitHub: boringssl"]
        GH_ASTERNET["AsterNet C++ source"]
    end

    subgraph "Build (repack.sh)"
        BSSL_BUILD["cmake → libssl.a + libcrypto.a"]
        XQUIC_BUILD["cmake → libxquic-static.a"]
        NG_BUILD["cmake → libnghttp2.a"]
    end

    subgraph "Repack"
        BSSL_SO["→ libssl.so + libcrypto.so"]
        XQUIC_SO["→ libxquic.so"]
        NG_SO["→ libnghttp2.so"]
    end

    subgraph "APK"
        ALL_SO["6 independent .so files"]
    end

    GH_XQUIC --> XQUIC_BUILD
    GH_NG --> NG_BUILD
    GH_BSSL --> BSSL_BUILD
    BSSL_BUILD --> BSSL_SO
    XQUIC_BUILD --> XQUIC_SO
    NG_BUILD --> NG_SO
    GH_ASTERNET --> ALL_SO
    BSSL_SO --> ALL_SO
    XQUIC_SO --> ALL_SO
    NG_SO --> ALL_SO
```

## Directory Layout

```text
AsterNet/
├── sdk/                        # Deliverable SDK components
│   ├── cxx/                    # C++ core library
│   │   ├── public/asternet/    #   Public C ABI headers
│   │   ├── client.{h,cpp}      #   Client facade
│   │   ├── abi/abi.cpp         #   C ABI implementation
│   │   ├── engine/             #   HTTP engines (h1, h2, quic)
│   │   └── platform/           #   OS abstractions
│   ├── android/                # Android JNI + AAR
│   ├── ios/                    # iOS (C ABI + Swift wrapper)
│   ├── harmonyos/              # HarmonyOS (NAPI + ArkTS)
│   └── third_party/            # Third-party build orchestration
├── examples/                   # Runnable examples (per-platform)
├── docs/                       # Documentation
└── scripts/                    # Build & release scripts
```

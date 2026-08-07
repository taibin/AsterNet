# xquic ↔ quic-go 兼容性说明

本测试服务器用 **quic-go v0.61**（Go 实现）提供 HTTP/3 对端，而 `asternet` 的 C++ 客户端基于 **xquic**（阿里，C 实现 + boringssl）。两者是**不同的 QUIC/HTTP3 实现**，跨实现对接存在若干兼容性要点，实测前需留意。

## 一、协议层兼容矩阵

| 层 | xquic | quic-go v0.61 | 兼容性 |
|---|---|---|---|
| QUIC 版本 | QUICv1(RFC9000) + v2 + 历史 draft-29 | QUICv1 + v2 | ✅ 默认 QUICv1 兼容；**注意 xquic 不能用 draft-29** |
| TLS | TLS 1.3（boringssl，RFC 9001） | TLS 1.3（Go crypto/tls，RFC 9001） | ✅ 普通握手兼容；0-RTT 脆弱（见下） |
| HTTP/3 | RFC 9114 | RFC 9114 | ✅ |
| QPACK | RFC 9204，动态表默认开 | RFC 9204，动态表默认开 | ✅；超大头部注意 `MaxHeaderBytes` |
| ALPN | `h3` | `h3` | ✅（见 `tests/test_h3_get.cpp`） |

## 二、已知风险点与规避

### 1. QUIC 版本必须统一为 RFC 9000 (v1)

xquic 历史版本默认走 IETF QUIC draft-29；quic-go v0.61 默认且仅支持 RFC 9000 v1（及 v2）。若 xquic 端用 draft-29，**握手在版本协商阶段就会失败**。

- C++ 侧：`xqc_conn_settings_t` 用 `XQC_CONN_SETTINGS_DEFAULT` 模板，确认其 QUIC 版本为 v1；如需显式指定，在 transport parameters 里设 `xqc_quic_proto_version` 为 `QUIC_VERSION_1`（0x00000001）。
- Go 侧：`quic.Config{Versions: []quic.VersionNumber{1}}`（本 server 未显式设，用 quic-go 默认 v1）。

### 2. 0-RTT：POC 阶段建议禁用

0-RTT 依赖 TLS session ticket + transport parameters 的跨实现互编解码。boringssl 与 Go crypto/tls 的 NewSessionTicket/PSK 编码存在历史差异，跨实现 0-RTT 偶发握手失败或参数丢失。

- 规避：先验证 **1-RTT** 普通握手 + 单请求成功，再开 0-RTT。
- C++ 侧：`save_session_cb` / `save_tp_cb`（见 `tests/test_h3_get.cpp`）持久化后二次连接复用；若复用失败会自动降级 1-RTT，不致命。

### 3. 连接迁移 / NAT 重绑定

两端都支持连接迁移（CID），但本地 loopback 测试不触发迁移。若在弱网/NAT 环境实测，注意：

- quic-go 对 CID 校验严格，迁移后新路径需通过 PATH_CHALLENGE/RESPONSE。
- xquic 客户端迁移后若 server 拒收，检查 server 侧 `quic.Config` 是否允许 `DisablePathMTUDiscovery` 等无关项，真正相关的是 CID 长度（见下）。

### 4. CID 长度

RFC 9000 允许 1–20 字节。xquic 默认 8–10，quic-go 默认 8，落在兼容区间。C++ 客户端发起 Initial 的 CID 由客户端决定，server 端用客户端 CID 或自选短 CID，无冲突。

### 5. Stateless Reset / 连接异常

quic-go 默认**不**响应 Stateless Reset Token；若 xquic 客户端侧发送 RESET 后立即重连，server 端连接会被静默清理。本地测试出现连接被关，直接重连即可，不要当成协议错误。

### 6. Idle Timeout

quic-go 默认 `MaxIdleTimeout = 30s`。若 C++ 客户端做长连接保活测试（IM/WebSocket 复用 QUIC 连接），需在 30s 内有流量或发 keep-alive，否则 server 主动关连接。如需更长 idle，C++ 侧在 transport params 设 `max_idle_timeout` 协商。

### 7. 证书校验

POC 阶段：C++ `cert_verify_cb` 返回 0（跳过，见 `tests/test_h3_get.cpp:194`），server 自签名 + curl `-k`，三方一致跳过校验。**生产环境必须**：server 换正式证书，C++ 客户端恢复标准校验，禁止 `cert_verify_cb` 无条件返回 0。

### 8. HTTP/3 头部 / SETTINGS

- 大头部：server `http3.Server.MaxHeaderBytes` 默认 `http.DefaultMaxHeaderBytes`（1MB），足够；C++ 侧若发送超大 cookie/authorization，注意客户端编码与 server 上限。
- SETTINGS 帧：两端都发，GOSUM 等 setting 一致；目前无已知不兼容 setting。

## 三、测试验证顺序建议

1. **server 自测**：`go run ./cmd/h3client`（quic-go ↔ quic-go，已验证 `proto=HTTP/3.0 alpn=h3`）——确认 server 本身 H3 可用。
2. **curl H1/H2**：`curl -k https://localhost:9443/`（已验证）——确认 TCP 降级通道可用。
3. **curl H3（若已编译）**：`curl --http3 -k https://localhost:8443/`——第三方实现交叉验证。
4. **C++ xquic 客户端**：`./test_h3_get localhost /` 或 `test_quic_engine` 对接 8443——真正的 xquic ↔ quic-go 跨实现验证。若失败，按上表 1–8 逐项排查，首选 QUIC 版本与 0-RTT。

## 四、当前已验证状态

| 链路 | 结果 |
|---|---|
| quic-go client → quic-go server (8443/H3) | ✅ HTTP/3.0 / h3 |
| curl → server (9443/H2) | ✅ HTTP/2.0 / h2 |
| curl → server (9443/H1.1) | ✅ HTTP/1.1 / http/1.1 |
| xquic client → quic-go server | ⏳ 待 C++ 端实测（注意 QUIC 版本与 0-RTT） |

## 五、参考

- xquic: https://github.com/alibaba/xquic
- quic-go: https://github.com/quic-go/quic-go
- RFC 9000 (QUIC v1) / 9001 (TLS 1.3 for QUIC) / 9114 (HTTP/3) / 9204 (QPACK)
- curl HTTP/3 编译: https://github.com/curl/curl/blob/master/docs/HTTP3.md

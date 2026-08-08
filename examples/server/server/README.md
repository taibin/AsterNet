# AsterNet 测试服务器（HTTP/1.1 + HTTP/2 + HTTP/3）

为 `asternet`（基于 xquic 的 C++ 网络库）提供三协议对端，验证自研客户端端到端连通性。同一 handler 挂在两个监听器上，便于对比同一请求在 H1/H2/H3 下的行为。

## 端口约定

| 监听器 | 地址 | 协议栈 | ALPN | 用途 |
|---|---|---|---|---|
| H3 | `https://localhost:8443` | HTTP/3 over QUIC（UDP） | `h3` | QUIC/HTTP3 主通道 |
| H1/H2 | `https://localhost:9443` | HTTP/1.1 + HTTP/2 over TLS（TCP） | `h2`, `http/1.1` | 降级通道，自动协商 |

端口选择与 `docs/POC_PROGRESS.md`（"server 监听 127.0.0.1:8443"）和 `tests/test_quic_engine.cpp`（`127.0.0.1:8443`）对齐。

## 启动

```bash
cd server
go run main.go            # 首次运行自动生成自签名证书 cert.pem / key.pem
```

启动后日志示例：

```
[H3]   监听 https://localhost:8443 (UDP/QUIC, ALPN: h3, TLS 1.3)
[H1/H2] 监听 https://localhost:9443 (TCP, ALPN: h2, http/1.1)
```

> 证书 SAN 含 `localhost / 127.0.0.1 / ::1`，自签名为 ECDSA-P256，有效期 365 天。`-k` 让 curl 跳过校验，C++ 客户端 POC 阶段 `cert_verify_cb` 同样跳过。

## curl 测试

### HTTP/1.1 与 HTTP/2（系统自带 curl 即可）

```bash
curl -k https://localhost:9443/                # 自动协商（macOS curl 默认 h2）
curl --http1.1 -k https://localhost:9443/      # 强制 HTTP/1.1
curl --http2 -k https://localhost:9443/         # 强制 HTTP/2
```

响应是 JSON，含本次协商出的协议，便于确认：

```json
{
  "proto": "HTTP/2.0",
  "alpn": "h2",
  "method": "GET",
  "host": "localhost:9443",
  "path": "/",
  ...
}
```

### HTTP/3（需编译 H3 支持的 curl）

系统自带 curl 通常**未**链接 QUIC 库，`curl --http3` 会报：

```
curl: option --http3: the installed libcurl version doesn't support this
```

两种方式验证 H3：

**方式 A —— 编译带 HTTP/3 的 curl**（可选，真正模拟 curl 客户端）：

```bash
# 方式一：Cloudflare 维护的 quiche + curl
git clone --recursive https://github.com/cloudflare/quiche.git
cd quiche
cargo build --release --manifest-path=quiche-core/Cargo.toml
# 然后 configure curl 时带 --with-openssl=$PWD/quiche/deps/boringssl --with-quiche=$PWD/quiche

# 方式二：ngtcp2 后端（curl 官方推荐）
# 参见 https://github.com/curl/curl/blob/master/docs/HTTP3.md
```

编译完成后：

```bash
curl --http3 -k https://localhost:8443/
curl --http3-only -k https://localhost:8443/
```

**方式 B —— 用仓库自带的 Go H3 客户端**（最省事，无需编译 curl）：

```bash
cd server
go run ./cmd/h3client      # 直连 8443，输出 proto=HTTP/3.0 alpn=h3
```

## 文件结构

```
server/
├── main.go                # 三协议 server 入口（H3 + H1/H2）
├── go.mod / go.sum        # 依赖：github.com/quic-go/quic-go v0.61
├── cmd/h3client/main.go   # 辅助 H3 客户端（验证用，InsecureSkipVerify）
├── cert.pem / key.pem     # 首次运行生成（已 gitignore，不入库）
└── README.md / COMPATIBILITY.md
```

## 与 C++ 客户端对接

| 字段 | 值 |
|---|---|
| host | `localhost` 或 `127.0.0.1` |
| port | `8443`（H3）/ `9443`（H1/H2） |
| path | `/` 或 `/index.html`（server 均路由到同一 handler） |
| ALPN | `h3`（见 `tests/test_h3_get.cpp`） |
| 证书校验 | POC 跳过（`cert_verify_cb` 返回 0） |

跨实现兼容性（xquic ↔ quic-go）注意事项见 `COMPATIBILITY.md`。

# AsterNet C++ 跨平台统一网络库方案

> 多端统一的 C++ 网络核心，对标阿里 XQUIC + ACCS / 腾讯 Mars
> 版本：v1.0 ｜ 日期：2026-07-27 ｜ 定位：与 `NETWORK_ARCHITECTURE_UPGRADE_PLAN.md`（Java 层渐进升级）并列的第二路线

---

## 0. 摘要（TL;DR）

本方案采用淘宝/阿里（XQUIC + ACCS）与腾讯 Mars 的路线：**用 C++ 实现一套跨平台网络核心，Android / iOS（及未来鸿蒙/PC）共用同一套传输、连接、DNS、探测、监控逻辑，各端只保留极薄的桥接壳**。核心选型 **基于阿里 XQUIC（开源 Apache-2.0，C 实现 QUIC/HTTP3）** 作为传输引擎，自研 C++ 网络框架（连接管理 / 智能 DNS / SDT 探测 / 监控 / 协议适配）收口其上，对外以**稳定 C ABI** 暴露，规避 C++ ABI 不兼容。

与上一套 Java 层方案的关系：**两者可融合落地**。C++ 核心成熟后，作为上一套 `NetworkEngine` SPI 的一个实现接入，业务层 `NetworkClient` API 不变；Java/Swift 层逐步退化为薄壳。落地采用**六阶段渐进迁移**：先 P0 速赢 → C++ 核心 POC → 短连接切入 → 长连接迁入 → 多端统一 → 旧 OkHttp 层下线。

核心收益：**协议自主可控（QUIC/连接迁移/0-RTT）、多端一致、长短板连接统一治理、可观测**；核心代价：**C++ 投入大、native 稳定性治理重、包体积增加**。

---

## 1. 背景与动机

### 1.1 为什么走 C++ 跨平台

现有 `AsterNet-net` 是纯 Java/Kotlin（OkHttp 4.9.1 + Retrofit）单端库，协议止步 HTTP/2，且 HttpDNS / 耗时埋点 / 401 拦截"写了未接线"（详见 `NETWORK_ARCHITECTURE_UPGRADE_PLAN.md` §1.4）。若仅做 Java 层升级，存在三个结构性问题：

1. **多端重复实现**：Android（OkHttp）与 iOS（URLSession/Alamofire）两套逻辑，能力难以对齐，弱网策略/DNS 选优/监控口径各自维护、长期漂移。
2. **协议栈受限于平台库**：OkHttp / URLSession 的 QUIC/HTTP3 支持滞后且不可定制，连接迁移、拥塞算法、0-RTT 策略无法自主调优。
3. **长连接与短连接割裂**：IM 走自研 WebSocket，与 HTTP 无统一连接/调度/监控层，跨端更难统一。

C++ 跨平台核心一次实现、多端复用，是头部厂商（阿里 XQUIC+ACCS、腾讯 Mars、字节 TTNet 基于 Cronet）验证过的路线。

### 1.2 业界 C++ 跨平台网络库对比

| 维度 | 阿里 XQUIC + ACCS | 腾讯 Mars | Google Cronet | 自研（本方案） |
|---|---|---|---|---|
| 语言 | C（XQUIC）/ C++（ACCS） | C++ | C++ | C++（核心）+ C ABI |
| 定位 | QUIC/HTTP3 传输引擎 + 统一长连接通道 | 完整跨平台网络库（STN+LongLink+SDT） | Chromium 网络栈抽取 | 完整跨平台网络库 |
| QUIC/HTTP3 | ✅ 自研（IETF QUIC，含迁移/0-RTT/FEC/BBR） | ✅ 自研接入 | ✅ 成熟 | ✅ 基于 XQUIC |
| 长短连接统一 | ✅ ACCS 长连接为主 | ✅ STN+LongLink | ❌ 仅短连接 | ✅ 统一治理 |
| 智能 DNS + IP 选优 | ✅（HttpDNS 同源） | ✅ | ❌ | ✅ |
| 网络探测 SDT | ✅ | ✅ 独立模块 | 部分 | ✅ |
| 跨端 | 移动端为主 | 移动端 + Win/Mac/Linux | 全平台 | 移动端为主，可扩 |
| 可定制性 | 高（开源可改） | 高（开源） | 中（体积大、定制重） | 最高 |
| 开源协议 | Apache-2.0（可商用） | MIT | BSD | — |
| 维护方 | 阿里 | 腾讯 | Google | 自建 |

### 1.3 路线选择结论

- **传输引擎**：不自研 QUIC（成本极高、风险大），**基于阿里 XQUIC**——与现有阿里 HttpDNS 同源生态、Apache-2.0 可商用可改、支持连接迁移/0-RTT/FEC/BBR、可裁剪。
- **网络框架**：**自研 C++ 框架**（连接管理 / DNS / SDT / 监控 / 协议适配），参考 Mars 的 STN+LongLink+SDT 模块化思想。不直接二次开发 Mars（C++ 代码陈旧、与 XQUIC 协议栈不匹配、改造量等同重写）。
- **桥接**：**稳定 C ABI** 对外暴露，Android JNI / iOS ObjC++ 各做薄壳，规避 C++ ABI 跨编译器/版本不兼容。

---

## 2. 目标架构

### 2.1 设计原则

1. **核心跨平台、端侧薄壳**：所有网络逻辑在 C++ 核心，端侧仅桥接 + 业务协议封装。
2. **C ABI 隔离**：对外 `extern "C"` 稳定接口，内部 C++ 自由演进。
3. **协议自主可控**：QUIC/HTTP3 基于 XQUIC，可定制拥塞/迁移/0-RTT 策略。
4. **长短连接统一**：短连接（HTTP）与长连接（IM）复用同一连接池、调度、监控。
5. **可用性优先**：QUIC → HTTP/2 → HTTP/1.1 智能降级，底线不破。
6. **数据驱动 + 可观测**：SDT 主动探测驱动策略，全链路埋点统一口径。
7. **与现有库平滑共存**：C++ 核心作为 `NetworkEngine` SPI 实现接入，灰度切流，可回滚。

### 2.2 总体分层架构

```
┌─────────────── Android ───────────────┐ ┌─────────────── iOS ───────────────┐
│ Kotlin 业务 (Retrofit/suspend API)     │ │ Swift 业务 (async/await API)       │
│ netsdk-android (Kotlin 薄壳)           │ │ netsdk-ios (Swift 薄壳)            │
│        ↕ JNI                           │ │        ↕ ObjC++                    │
└────────────────┬───────────────────────┘ └────────────────┬──────────────────┘
                 │                          C ABI (extern "C")│
┌────────────────┴─────────────────────────┴──────────────────┴─────────────────┐
│                        C++ 跨平台网络核心 (hs-netcore)                          │
│  ┌──────────────────────────────────────────────────────────────────────────┐ │
│  │ ① 接口层 (C ABI 导出 + 内部 Facade)                                       │ │
│  │    asternet_client_create/request_sync/...  ·  句柄式 ·  C ABI             │ │
│  ├──────────────────────────────────────────────────────────────────────────┤ │
│  │ ② 协议适配层 (protocol)                                                  │ │
│  │    网关协议(类MTOP可选) · IM 长连接协议(protobuf) · 序列化               │ │
│  ├──────────────────────────────────────────────────────────────────────────┤ │
│  │ ③ 请求编排层 (orchestrator)                                              │ │
│  │    拦截器链 · 重试/降级调度 · 请求合并 · 结果封装                         │ │
│  ├──────────────────────────────────────────────────────────────────────────┤ │
│  │ ④ 连接管理层 (connection)                                                │ │
│  │    长短连接统一池 · 多路复用 · 连接迁移 · 0-RTT · 预连接 · 心跳保活       │ │
│  ├──────────────────────────────────────────────────────────────────────────┤ │
│  │ ⑤ 传输引擎层 (engine) ── 基于 XQUIC                                      │ │
│  │    HTTP/3(QUIC) · HTTP/2 · HTTP/1.1 · 协议选择器(降级熔断)              │ │
│  │    TLS · 拥塞控制(BBR/Cubic) · Datagram                                  │ │
│  ├──────────────────────────────────────────────────────────────────────────┤ │
│  │ ⑥ 基础设施层                                                             │ │
│  │    智能 DNS(HttpDNS+LocalDNS+IP探测选优+预解析) · SDT 网络探测/弱网判定  │ │
│  │    监控埋点(统一口径) · TaskRunner线程模型 · 平台适配(epoll/kqueue/IOCP) │ │
│  └──────────────────────────────────────────────────────────────────────────┘ │
└────────────────────────────────────────────────────────────────────────────────┘
```

### 2.3 C++ 核心模块划分

| 模块 | 职责 | 关键组件（规划） |
|---|---|---|
| `engine` | 传输引擎，封装 XQUIC + HTTP/2/1.1 | `Engine`、`QuicSession`、`Http3Stream`、`ProtocolSelector`、`CircuitBreaker` |
| `connection` | 长短连接统一管理 | `ConnectionPool`、`Multiplexer`、`ConnectionMigration`、`ZeroRtt`、`Prefetcher`、`Keepalive` |
| `dns` | 智能 DNS | `SmartDnsResolver`、`IpProber`、`IpRanker`、`PrefetchScheduler`、`DnsCache` |
| `sdt` | 网络探测与弱网 | `QualityProber`、`WeakNetDetector`、`DynamicConfig` |
| `orchestrator` | 请求编排 | `InterceptorChain`、`RetryPolicy`、`RequestCoalescer`、`ResponseWrapper` |
| `protocol` | 业务协议适配 | `GatewayProtocol`(可选类MTOP)、`ImProtocol`(protobuf)、`Codec` |
| `monitor` | 监控埋点 | `MetricsCollector`、`Span`、`Reporter` |
| `platform` | 平台适配 | `EventLoop`(epoll/kqueue)、`TaskRunner`、`Thread`、`Log`、`CrashHandler` |
| `abi` | C ABI 导出 | `asternet_*` 句柄式接口 |

### 2.4 跨端桥接设计

#### 2.4.1 C ABI 规约（核心，决定稳定性）

```c
// 句柄式 + POD 参数 + 回调指针，绝不跨边界传 STL 容器
typedef struct asternet_client asternet_client_t;
typedef struct asternet_request asternet_request_t;

// 创建/销毁
asternet_client_t* asternet_client_create(const asternet_client_config_t* cfg,
                                          asternet_result_t* out_error);
void               asternet_client_destroy(asternet_client_t* c);

// 异步请求（回调由指定线程抛回）
asternet_result_t asternet_client_request_sync(asternet_client_t* c,
                                                const asternet_request_t* req,
                                                uint8_t* out_body,
                                                size_t out_body_capacity,
                                                asternet_response_info_t* out_info);

// 长连接
// WebSocket 尚未纳入当前公共 ABI。
```

规约要点：
- **只传 POD / opaque handle**，字符串用 UTF-8 + 长度的 buffer；**禁止 `std::string`/`std::vector` 跨 ABI**（布局不稳定）。
- **句柄式生命周期**：create 返回不透明指针，destroy 释放；端侧持有句柄，内部 C++ 对象由核心管理。
- **回调线程明确**：回调在端侧指定的 `TaskRunner`（通常主线程）触发，避免端侧线程安全问题。
- **版本协商**：ABI 带 `abi_version`，核心与壳版本不匹配时拒绝初始化，防止内存布局错位崩溃。

#### 2.4.2 Android 壳（Kotlin + JNI）

- `netsdk-android` module：JNI 层（C++ ↔ C ABI ↔ Java）+ Kotlin 封装。
- 对业务暴露与现有 `NetworkClient` 一致的 API（suspend 协程），内部调 JNI。
- 协程桥接：JNI 回调投递到 `Dispatchers.Main`，转 `suspendCancellableCoroutine`。

#### 2.4.3 iOS 壳（Swift + ObjC++）

- `netsdk-ios` framework：ObjC++ 桥接层（`.mm`，C++ ↔ C ABI ↔ ObjC）+ Swift 封装。
- 对业务暴露 `async/await` API，内部通过 ObjC++ 调 C ABI。
- 回调抛回 `MainActor`。

### 2.5 线程模型

```
┌─────────────────────────────────────────────────────┐
│ 端侧主线程 (Kotlin/Swift 业务回调)                   │
│   ↑ 结果回调 (通过 TaskRunner 投递)                  │
├─────────────────────────────────────────────────────┤
│ C++ 核心                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────┐ │
│  │ Network 线程  │  │ Worker 线程池 │  │ Callback   │ │
│  │ (1, 事件循环) │  │ (N, CPU密集)  │  │ Dispatcher │ │
│  │ epoll/kqueue │  │ 加解密/序列化 │  │ (投递主线程)│ │
│  │ socket I/O    │  │ DNS解析/探测  │  │            │ │
│  └──────────────┘  └──────────────┘  └────────────┘ │
└─────────────────────────────────────────────────────┘
```

- **Network 线程**（1 个）：跑 `EventLoop`（Android/Linux 用 epoll，iOS 用 kqueue），所有 socket I/O 与 XQUIC 事件在此线程，无锁高性能。
- **Worker 线程池**（N 个）：CPU 密集任务（TLS 加解密、protobuf 序列化、DNS 解析、IP 探测），避免阻塞网络线程。
- **Callback Dispatcher**：结果通过 `TaskRunner` 投递回端侧主线程，保证回调线程安全。
- **无全局锁热点**：连接/请求用 per-connection 锁或无锁队列，网络线程不阻塞。

### 2.6 异步模型

- **核心内部**：回调 + `Promise/Future`（不依赖 C++20 协程，规避 NDK/编译器差异风险）。
- **对外**：端侧按语言习惯封装——Android `suspend`，iOS `async/await`。

---

## 3. 核心机制设计

### 3.1 传输引擎（基于 XQUIC）

- **集成方式**：以 XQUIC 为子模块（git submodule 或源码引入），`engine` 模块封装其 `xqc_engine_create` / `xqc_h3_request_create` 等 API。
- **协议栈**：HTTP/3（QUIC）为主，HTTP/2 / HTTP/1.1 作为降级（XQUIC 不提供 H2/H1.1，需在 `engine` 层用轻量实现或复用平台栈兜底）。
- **拥塞控制**：支持 BBR / Cubic 可切换，弱网优先 BBR。
- **可定制**：连接迁移、0-RTT、FEC、Datagram 等通过 XQUIC 配置项开启。

### 3.2 智能协议降级

```
请求 → ProtocolSelector
  ├─ 首选 HTTP/3(QUIC)：引擎支持且近期 QUIC 成功率达标
  ├─ 次选 HTTP/2：QUIC 失败/被熔断时回退
  └─ 兜底 HTTP/1.1：极端弱网或 QUIC/H2 均异常
```
- **host 级熔断器**：QUIC 连续失败 N 次自动降级 H2，冷却后探活恢复（**修正现有 `connectionPool.evictAll()` 暴力清池的误伤问题**）。
- 降级阈值由 `sdt` 实时成功率/RTT/丢包率计算。

### 3.3 长短连接统一管理（ACCS/Mars 思路核心）

- **统一连接池**：短连接（HTTP/3 stream）与长连接（IM）复用同一 QUIC 连接的多路复用能力，一条 QUIC 连接承载多业务流。
- **连接迁移**：QUIC Connection ID 与四元组解耦，`NetworkReceiver` 监听网络切换 → 通知 `ConnectionMigration` → 平滑迁移，在途请求不丢；迁移失败降级重连 + 幂等重试。
- **0-RTT**：TLS Session 复用，二次建连零握手；**仅对幂等请求开启**，防重放。
- **预连接**：启动/切前台时对关键域名预建连，首屏零握手。
- **心跳保活**：长连接自适应心跳（弱网缩短、强网拉长），迁移自现有 WS 30s ping 逻辑。

### 3.4 智能 DNS

```
解析 → SmartDnsResolver
  ├─ 1. HttpDNS（主，防劫持）   ← 修正现有 OkHttpDns 未接线问题，迁入 C++ 核心
  ├─ 2. LocalDNS（兜底）
  ├─ 3. IP 探测测速（IpProber）：候选 IP 测 RTT/丢包，IpRanker 排序
  ├─ 4. 预解析调度（PrefetchScheduler）
  └─ 5. 缓存（内存 LRU + 持久化），TTL + 失效兜底
```
- 跨端共享同一选优逻辑，Android/iOS DNS 行为一致。

### 3.5 网络探测 SDT + 弱网

- `QualityProber`：周期主动探测 RTT/丢包/带宽 + 被动业务数据，输出**网络质量评分**。
- `WeakNetDetector`：评分低于阈值判定弱网。
- `DynamicConfig`：弱网动态收紧超时、降并发、启用请求合并、优先 QUIC+0-RTT、增关键请求重试。

### 3.6 业务协议适配

- **IM 协议**：现有 WebSocket + protobuf 迁入 `protocol` 模块。**迁移期保持 WebSocket over QUIC**（平滑、协议不变）；远期可演进为基于 QUIC Datagram 的自定义长连接协议（更低延迟）。
- **网关协议（可选）**：若后端有统一网关，引入类 MTOP 的协议层（统一签名/加密/压缩/错误码），各业务 API 收口到网关。无统一网关则各 API 直连，此项跳过。

### 3.7 监控埋点（统一口径）

- `MetricsCollector`：每次请求采集 DNS/建连/TLS/首字节/总耗时、协议、成功/失败码、重试/降级路径、迁移事件。
- **跨端统一口径**：Android/iOS 上报字段一致，大盘可横向对比。
- 支持 AB 实验（QUIC vs H2 首屏/成功率）。

---

## 4. 跨平台工程化

### 4.1 构建系统（CMake）

- C++ 核心用 **CMake**（跨平台事实标准），统一编译 XQUIC + 自研代码。
- **Android**：CMake + NDK，AGP `externalNativeBuild` 集成，产出 `.so` 打入 `.aar`。
- **iOS**：CMake + toolchain 生成 `.xcframework`（arm64 真机 + arm64/x86_64 模拟器）。
- **CI**：统一编译产出制品（.aar / .xcframework），各端消费，保证多端二进制一致来源。

### 4.2 产物与 ABI 裁剪

| 平台 | ABI | 说明 |
|---|---|---|
| Android | arm64-v8a + armeabi-v7a | 主流覆盖；x86_64 仅调试 |
| iOS | arm64(真机) + arm64/x86_64(模拟器) | xcframework |

- XQUIC 按需裁剪（关闭不用的 FEC/Datagram 扩展）。
- **LTO + strip** 减体积；release 开启。
- **动态库**（.so / .dylib-in-framework）：多 module 共享、便于热更；体积大于静态库但避免重复打包。推荐动态库。

### 4.3 Native Crash 治理（关键）

- 集成 **breakpad / crashpad** 捕获 native 崩溃，dump 上报。
- **符号表管理**：每次发版归档 `.so`/`.dSYM` 符号表，CI 自动上传，崩溃栈可还原。
- **崩溃率门禁**：灰度 native 崩溃率不高于基线才放全量。
- ASan/TSan 在 debug 构建常态化跑，提前发现内存/线程问题。

### 4.4 包体积控制

- XQUIC 裁剪 + ABI 限定 + LTO/strip。
- 评估动态库压缩后体积，目标增量可控（参考 XQUIC 移动端约数 MB 级）。
- 按渠道按需打包（低端机渠道可仅 arm64 + 关闭 FEC）。

---

## 5. 各端薄壳设计

### 5.1 Android（`netsdk-android`）

```
Kotlin 业务 API (suspend) ── 同现有 NetworkClient，业务零改动
        ↓
Kotlin 封装层 (参数组装 / 协程桥接)
        ↓ JNI
C++ ABI (asternet_*)
        ↓
hs-netcore
```
- 对外保持 `NetworkClient` / `INetworkService` 接口不变，业务无感切换。
- 现有 OkHttp/Retrofit 作为**过渡兼容层**保留，灰度期与 C++ 核心���存。

### 5.2 iOS（`netsdk-ios`）

```
Swift 业务 API (async/await)
        ↓
Swift 封装层
        ↓ ObjC++
C++ ABI (asternet_*)
        ↓
hs-netcore
```
- 新建 Swift 网络库，API 风格与 Android 对齐，逻辑共用 C++ 核心。

---

## 6. 与现有库的关系与迁移路径

> 六阶段，每阶段独立可上线、可灰度、可回滚。C++ 核心作为 `NetworkEngine` SPI 实现接入，业务 API 全程不变。

### 阶段 0：P0 速赢 + C++ 核心 POC（并行）
- **P0 速赢（先行，不依赖 C++）**：接入现有 HttpDNS、恢复 `OKHttpEventListener` 埋点、接入 `LogoutInterceptor` 401（详见 Java 方案 §1.4）。建立可观测/防劫持/登录态基线。
- **C++ POC**：XQUIC 集成跑通（Android NDK 编译 + 简单 QUIC 请求），验证体积/性能/兼容性/崩溃率；C ABI + JNI 桥接跑通 hello-world；确定线程模型与 EventLoop。
- **验证**：POC 通过可行性 gate，决定是否全量推进。

### 阶段 1：C++ 核心骨架 + 引擎 SPI 接入（Android 单端）
- 实现 `engine`/`connection`/`dns`/`orchestrator`/`monitor`/`abi` 骨架，`OkHttpEngine` 与 `XQUICEngine` 双引擎。
- 作为 `NetworkEngine` SPI 实现接入现有 `NetworkClient`，**灰度小流量**走 C++ 核心，影子流量对比一致性。
- **验证**：一致性、性能、崩溃率达标。

### 阶段 2：短连接全量切 C++ 核心（Android）
- 短连接（HTTP）全部走 C++ 核心，QUIC + 智能降级 + 智能 DNS + SDT 全开启。
- 旧 OkHttp 短连接层进入只读维护。
- **验证**：弱网首屏 P90 改善、成功率不降、降级链路可用。

### 阶段 3：长连接（IM）迁入 C++ 核心（Android）
- WebSocket + protobuf 迁入 `protocol` 模块，复用 QUIC 连接与监控。
- 统一长短连接治理。
- **验证**：IM 连接稳定性、切网迁移成功率、消息可达率。

### 阶段 4：iOS 接入 + 多端统一
- iOS `netsdk-ios` 开发，复用 C++ 核心，Swift API 对齐 Android。
- 两端 DNS/弱网/监控口径统一。
- **验证**：iOS 性能与稳定性达标、两端行为一致。

### 阶段 5：旧 OkHttp 层下线 + 持续优化
- Android 下线 OkHttp/Retrofit 短连接层，仅保留必要兼容。
- 持续优化：预连接命中率、请求合并、连接迁移覆盖、体积裁剪、拥塞算法调优。

### 里程碑（建议）

| 阶段 | 周期 | 关键交付 | 可量化收益 |
|---|---|---|---|
| 0 P0+POC | 4–6 周 | 速赢上线 + C++ POC | 可观测基线 + 可行性验证 |
| 1 核心骨架+SPI | 6–8 周 | C++ 核心 + 灰度接入 | 一致性验证 |
| 2 短连接全量 | 6–8 周 | QUIC+降级+DNS+SDT | 弱网首屏 P90↓ |
| 3 长连接迁入 | 6–8 周 | IM 统一治理 | 切网失败率↓、可达率↑ |
| 4 iOS 接入 | 8–10 周 | 多端统一 | 两端一致 |
| 5 下线+优化 | 持续 | OkHttp 下线 | 体积/性能持续优化 |

> 总周期约 9–12 个月（含 POC），视团队 C++ 投入与并行度而定。

---

## 7. 关键决策点（需拍板，含推荐）

| # | 决策点 | 选项 | 推荐 | 理由 |
|---|---|---|---|---|
| D1 | 目标端范围 | Android+iOS / 含鸿蒙 / 含 PC | **Android+iOS 起步** | 覆盖主战场，C++ 核心可扩展，避免初期摊大 |
| D2 | QUIC 引擎 | XQUIC / Cronet / 从零自研 | **XQUIC** | 开源可改、与 HttpDNS 同源、可裁剪、Apache-2.0 |
| D3 | 网络框架 | 自研 / 二次开发 Mars | **自研** | Mars 陈旧且与 XQUIC 不匹配，改造量等同重写 |
| D4 | 长连接协议 | 保持 WS over QUIC / 重构自定义协议 | **先 WS over QUIC，远期演进** | 平滑迁移、协议不变、风险低 |
| D5 | 统一网关协议层 | 引入类 MTOP / 各 API 直连 | **看后端** | 有统一网关则引入，无则跳过 |
| D6 | 投入模式 | 自建团队 / 外部协作 | **自建 C++ 核心团队** | 长期自主可控，需评估 C++ 人力 |
| D7 | 与 Java 方案关系 | 二选一 / 融合 | **融合** | C++ 核心作 Engine SPI 实现，Java 方案作为过渡与兜底 |

---

## 8. 风险与应对

| 风险 | 等级 | 应对 |
|---|:-:|---|
| C++ 投入大、周期长 | 高 | 六阶段渐进、每阶段可独立产出价值；POC 先验证 gate；可与 Java 方案并行兜底 |
| 团队 C++ 能力不足 | 高 | 评估/补充 C++ 人力；核心模块由资深 C++ 工程师主导；ASan/TSan/Code Review 把关 |
| Native 崩溃难定位 | 高 | breakpad/crashpad + 符号表管理 + 崩溃率门禁；ASan/TSan 常态化 |
| 包体积增大 | 中 | XQUIC 裁剪 + ABI 限定 + LTO/strip + 动态库 + 按渠道打包 |
| 部分机型 UDP/QUIC 受限 | 中 | 降级熔断兜底 H2/HTTP1.1；UDP 黑名单机型；崩溃率门禁 |
| ABI 不兼容崩溃 | 中 | C ABI 隔离 + 版本协商；禁止 STL 跨边界；CI 多端二进制一致性校验 |
| 0-RTT 重放攻击 | 中 | 仅幂等请求开启；服务端 nonce 校验 |
| 迁移期双栈维护成本 | 中 | 灰度切流、影子流量对比；旧层只读维护；明确下线时间点 |
| XQUIC 社区维护风险 | 低 | Apache-2.0 可自维护 fork；评估社区活跃度 |

---

## 9. 验证方式

- **功能**：每阶段单测（C++ 层 GoogleTest）+ 端侧 UI/集成测试；C ABI 一致性测试。
- **性能**：AB 实验，首屏 P50/P90/P99、建连耗时、成功率、切网失败率、IM 可达率。
- **弱网**：弱网实验室（丢包/延迟/抖动/带宽限速）+ 线上弱网样本，两端对比。
- **稳定性**：native 崩溃率/ANR 灰度门禁（不高于基线）。
- **多端一致性**：Android/iOS 同口径监控大盘，行为/指标对齐。
- **可观测**：统一大盘可查每阶段指标，收益可量化、问题可定位。

---

## 10. 总结

本方案对标阿里 XQUIC+ACCS / 腾讯 Mars，以 **C++ 跨平台核心 + 各端薄壳 + C ABI 隔离** 为骨架，**基于 XQUIC** 提供 QUIC/HTTP3/连接迁移/0-RTT，**自研 C++ 框架** 统一长短连接、智能 DNS、SDT 探测、监控，Android/iOS 共用一套逻辑。与 Java 层渐进方案**融合落地**：C++ 核心作为 `NetworkEngine` SPI 实现接入，业务 API 全程不变，六阶段渐进迁移、可灰度可回滚。

落地后可实现：协议自主可控、多端能力一致、长短连接统一治理、弱网与切网体验显著提升、可观测性统一。代价是 C++ 投入与 native 稳定性治理，需配套 C++ 团队与 crash 治理基建。

> 下一步建议：① 立即执行 P0 速赢三项（不阻塞）；② 启动 XQUIC POC（NDK 编译 + QUIC 请求 + JNI 桥接 hello-world + 体积/性能/崩溃率评估），POC 通过即进入阶段 1。

# XQUIC POC 选型评估清单

> 阶段 0 的可行性验证。POC 通过 → 进入阶段 1（核心骨架 + 引擎 SPI 接入）；不通过 → 评估 Cronet 作 Plan B。
> 时间盒：2–3 周。以下为可执行清单与 Pass/Fail 判据。

## 1. POC 目标

验证 **基于阿里 XQUIC** 作为传输引擎在移动端（Android 优先）的可行性，覆盖：编译集成、体积、性能、稳定性、兼容性、关键能力（连接迁移 / 0-RTT / 降级）。

**总判据**：以下所有 gate 达标 → Pass；任一 CRITICAL gate 未达 → Fail。

## 2. 前置环境

| 项 | 要求 |
|---|---|
| Android NDK | r25+（XQUIC 对 NDK 版本有要求，实测确认） |
| CMake | 3.18+（`brew install cmake`） |
| XQUIC 版本 | 锁定一个 release tag（如 v1.x），记录 commit，fork 备用 |
| targetSdk/minSdk | minSdk 21（与旧库一致） |
| ABI | arm64-v8a（POC 先单 ABI） |

## 3. 编译集成步骤

- [ ] 以 git submodule 引入 `third_party/xquic`（或源码拷贝），记录版本。
- [ ] XQUIC 依赖：boringssl / boringssl 静态库（XQUIC 文档要求的 TLS 栈），按其 `CMakeLists.txt` 编译。
- [ ] 在 `src/CMakeLists.txt` 开启 `ASTERNET_ENABLE_XQUIC=ON`，`add_subdirectory(third_party/xquic)`，`target_link_libraries(asternet-core PRIVATE xquic)`。
- [ ] 实现 `src/engine/quic_engine.{h,cpp}`：封装 `xqc_engine_create` / `xqc_h3_request_create`，跑通一个 HTTP/3 GET。
- [ ] NDK 编译产出 `libasternet-core.so`（含 XQUIC），无符号冲突、无链接错误。
- [ ] **gate**：编译通过，CI 可复现构建。

## 4. 体积基准

- [ ] 测量 `libasternet-core.so`（arm64-v8a, Release, LTO+strip）体积。
- [ ] 拆分：XQUIC + boringssl + 自研代码占比。
- [ ] **gate（CRITICAL）**：arm64-v8a 增量 ≤ 4 MB（压缩后）；超限需裁剪（关 FEC/Datagram 扩展）后再测。
- [ ] 评估 `armeabi-v7a` 增量（目标 ≤ 3 MB）。

## 5. 性能指标

在弱网实验室（丢包 2%/5%、延迟 100ms/300ms、抖动）与正常网络下，AB 对比 OkHttp(HTTP/2)：

| 指标 | gate（弱网） | gate（正常） |
|---|---|---|
| 首屏 P90 | 较 H2 改善 ≥ 20% | 不劣化 |
| 建连耗时 P90 | ≤ H2 的 70%（0-RTT 二次建连接近 0） | ≤ H2 |
| 请求成功率 | ≥ H2 | ≥ H2 |
| 0-RTT 命中率 | 二次建连 ≥ 90% | — |

- [ ] **gate（CRITICAL）**：弱网首屏 P90 改善 ≥ 20%，且正常网络不劣化。
- [ ] 0-RTT 与连接迁移的功能性验证（见 §7）。

## 6. 稳定性 gate

- [ ] 灰度小样本（内部/测试包）native 崩溃率 ≤ 基线（基线 = 当前线上 native 崩溃率）。
- [ ] ASan/TSan 跑核心 + XQUIC 集成路径，无内存/线程错误。
- [ ] breakpad 接入，崩溃 dump 可还原符号。
- [ ] **gate（CRITICAL）**：崩溃率不高于基线，ASan/TSan 零报错。
- [ ] 长跑测试：连续请求 1h，无内存泄漏（valgrind/LeakSanitizer）。

## 7. 功能验证清单

- [ ] **连接迁移**：请求进行中切换 WiFi↔蜂窝，连接不断、在途请求成功（QUIC Connection ID 迁移）。
- [ ] **0-RTT**：二次建连耗时接近 0；仅对幂等请求开启，验证非幂等请求不走 0-RTT。
- [ ] **协议降级**：模拟 QUIC 不可用（UDP 阻断），自动降级 HTTP/2 再 HTTP/1.1，成功率不降。
- [ ] **多路复用**：单 QUIC 连接并发多请求，无队头阻塞。
- [ ] **智能 DNS**：HttpDNS 解析 + IP 选优 + LocalDNS 兜底链路通畅（可与 §5 性能合并验证）。
- [ ] **网络切换**：`asternet_client_on_network_change` 触发迁移与重连，IM 长连接（阶段3）可达率达标。

## 8. 兼容性

- [ ] UDP 受限机型黑名单收集（部分国产 ROM 限 UDP），验证降级生效。
- [ ] minSdk 21~35 区间代表性机型（华为/小米/OPPO/vivo/三星）跑通。
- [ ] IPv6-only / 双栈环境验证。
- [ ] **gate**：UDP 黑名单机型自动降级，无功能性故障。

## 9. Pass/Fail 判定

| 维度 | 判定 |
|---|---|
| 编译集成 | 必须通过 |
| 体积 | arm64 ≤ 4MB（CRITICAL） |
| 性能 | 弱网首屏 P90 改善 ≥ 20%（CRITICAL） |
| 稳定性 | 崩溃率 ≤ 基线，ASan/TSan 零报错（CRITICAL） |
| 功能 | 连接迁移/0-RTT/降级/多路复用全部通过 |
| 兼容性 | UDP 黑名单机型自动降级 |

**全部 CRITICAL gate 达标 → Pass，进入阶段 1。**
**任一 CRITICAL gate 未达 → Fail，评估 Cronet 作 Plan B（对照跑同样 gate）。**

## 10. 产出物

- [ ] `third_party/xquic` 锁版本 + 编译脚本。
- [ ] `src/engine/quic_engine.{h,cpp}` POC 实现。
- [ ] 性能/体积/稳定性数据报告（含 AB 对比）。
- [ ] 兼容性机型矩阵。
- [ ] Pass/Fail 决策文档，附决策依据与下一步计划。

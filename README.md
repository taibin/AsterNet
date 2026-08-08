# AsterNet

跨平台 C++ 网络核心，支持 HTTP/1.1、HTTP/2 和 HTTP/3，附带 Android Network Lab Demo。

## 快速开始（Android）

```bash
# 1. 设置第三方依赖
./scripts/setup-third-party.sh --local third_party/xquic third_party/nghttp2

# 2. 构建 Demo APK
./gradlew :examples:android:assembleDebug

# 3. 安装到设备
adb install -r examples/android/build/outputs/apk/debug/android-debug.apk
```

如果没有本地编译好的 xquic/nghttp2，需要先从 GitHub Releases 下载预编译库：

```bash
# 替换为实际的 Release URL
./setup-third-party.sh --url https://github.com/YOUR_ORG/asternet/releases/download/libs-v1
```

## Demo App 功能

AsterNet Network Lab 提供两个页面：

| Tab | 功能 |
|-----|------|
| **Presets** | 5 个预设场景，一键测试 HTTP/1.1、HTTP/2、HTTP/3 协议 |
| **Custom** | 自定义请求：自由编辑域名、端口、路径、方法、Headers、Body、超时 |

支持的方法：`GET` / `POST` / `PUT` / `DELETE`

支持的协议策略：`AUTO` / `HTTP/1.1` / `HTTP/2` / `HTTP/3`

## 第三方依赖管理

项目依赖三个 C/C++ 库，**源码不入仓库**（体积近 1GB），通过预编译 `.a` 文件分发：

| 库 | 用途 | 预编译大小 |
|----|------|-----------|
| nghttp2 | HTTP/2 协议 | 308 KB |
| xquic | HTTP/3 (QUIC) | 7 MB |
| BoringSSL | TLS 加密 | 41 MB (ssl + crypto) |

### 脚本说明

```bash
# 下载预编译库 + 头文件
./setup-third-party.sh --url <base_url>

# 从本地 xquic 构建目录拷贝（已有编译产物时）
./setup-third-party.sh --local path/to/xquic path/to/nghttp2

# 打包本地编译产物，准备上传 GitHub Releases
./package-third-party.sh
```

### 发布预编译库到 GitHub Releases

```bash
# 1. 打包
./package-third-party.sh

# 2. 创建 Release
gh release create libs-v1 third_party_release/* \
  --title "Prebuilt libs arm64-v8a" \
  --notes "nghttp2 + xquic + BoringSSL 预编译静态库"
```

## 构建

```bash
# 统一构建入口
./scripts/build.sh --android          # 仅 Android
./scripts/build.sh --android --debug  # Android Debug（可调试）
./scripts/build.sh --cxx              # C++ 核心 + 测试
./scripts/build.sh --cxx --test       # 编译并运行测试
./scripts/build.sh --all              # 全平台
```

### 桌面构建

需要 CMake ≥ 3.18、C++17、以及预编译的 nghttp2 和 BoringSSL：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

公网端到端测试工具 `build/tests/test_e2e` 和 `build/tests/test_protocol_switch` 默认不注册到 CTest；确认网络可用后用以下命令启用：

```bash
cmake -S . -B build -DASTERNET_REGISTER_NETWORK_TESTS=ON
```

## Android 构建选项

默认编译 HTTP/1.1 + HTTP/2。启用 HTTP/3 时设置 `ASTERNET_ENABLE_XQUIC=ON`（`demo-app/build.gradle` 中已默认开启）：

```bash
# 仅 HTTP/1.1 + HTTP/2（较快）
./gradlew :demo-app:assembleDebug

# 含 HTTP/3（需要 xquic + boringssl 预编译库）
# 已在 build.gradle 中默认配置 ASTERNET_ENABLE_XQUIC=ON
```

CMake 会自动从以下路径查找预编译库：

```
sdk/third_party/xquic/build-android-arm64/libxquic-static.a
sdk/third_party/xquic/third_party/boringssl/build-android-arm64/libssl.a
sdk/third_party/xquic/third_party/boringssl/build-android-arm64/libcrypto.a
sdk/third_party/nghttp2/build-android-arm64/libnghttp2.a
```

## ABI 兼容性

| 字段 | 值 |
|------|-----|
| ABI Version | `0x00010000` (Major=1, Minor=0) |
| minSdk | 21 (Android 5.0) |
| targetSdk | 36 |
| ABI | arm64-v8a |

## 目录结构

```text
sdk/                    ====== SDK ======
├── cxx/                C++ 核心库 → libasternet.so
│   └── public/asternet/ 公共 C ABI 头文件
├── android/            Android JNI 桥 → libasternet-jni.so + AAR
├── ios/                iOS SDK（占位）
├── harmonyos/          鸿蒙 SDK（占位）
└── third_party/        三方库独立构建（nghttp2/xquic/boringssl）

examples/               ====== 示例 & 测试 ======
├── android/            Android Demo App（预设场景 + 自定义请求）
├── cxx/                C++ 单元测试
├── server/             Go 测试服务器（HTTP/1.1 + HTTP/2 + HTTP/3）
└── ios/                iOS Example（占位）

scripts/                构建 & 发布脚本
docs/                   设计文档
```

## 架构

```
┌──────────────────────────────────────────────┐
│  Demo App (Java)                              │
│  ├─ Presets Tab  ── 预设场景                   │
│  └─ Custom Tab   ── 自定义请求表单              │
├──────────────────────────────────────────────┤
│  AsterNet.java  ── JNI ──► asternet_jni.cpp  │
├──────────────────────────────────────────────┤
│  C ABI (asternet_*)  ←──  asternet/asternet.h │
├──────────────────────────────────────────────┤
│  Client  ──►  ProtocolSelector               │
│                ├─ QuicEngine   (HTTP/3)       │
│                ├─ Http2Engine  (HTTP/2)       │
│                └─ Http1Engine  (HTTP/1.1)     │
│             降级链: H3 → H2 → H1              │
├──────────────────────────────────────────────┤
│  nghttp2  │  xquic  │  BoringSSL             │
└──────────────────────────────────────────────┘
```

## 限制

- 当前为同步请求接口，尚未提供异步回调、WebSocket 或连接池。
- HTTPDNS 仅有 Resolver SPI/缓存设计，未绑定具体服务端。
- Android Demo 已适配系统 CA 证书；其他平台需自行提供 trust store。
- HTTP/3 需要 UDP/443 可达的网络环境。

## 许可证

Apache License 2.0。第三方依赖的许可证以各自目录中的原始文件为准。

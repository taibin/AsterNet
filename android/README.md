# Android 集成与测试指南

AsterNet 网络核心 Android 端使用说明：可切换 HTTP/1.1、HTTP/2、HTTP/3，记录整体耗时。

## 1. 目录结构

```
android/
├── CMakeLists.txt                              # JNI 壳构建（链接 asternet-core）
├── jni/asternet_jni.cpp                           # JNI 桥接（含 nativeRequestSync）
├── src/main/java/com/AsterNet/net/
│   ├── AsterNetCore.kt                            # Kotlin 壳（requestSync + Response 解析）
│   └── demo/MainActivity.kt                    # Demo：三按钮切换 H1/H2/H3
├── src/main/res/layout/activity_main.xml       # Demo 布局
└── README.md                                   # 本文件
```

## 2. 构建 .so（NDK 交叉编译 arm64-v8a）

C++ 核心需先交叉编译 boringssl + xquic + nghttp2，再编译 asternet-core + JNI 壳。

```bash
cd asternet
NDK=~/Library/Android/sdk/ndk/27.2.12479018
TOOLCHAIN=$NDK/build/cmake/android.toolchain.cmake
CMAKE=~/Library/Android/sdk/cmake/3.22.1/bin/cmake

# 2.1 boringssl (arm64)
BSSL=third_party/xquic/third_party/boringssl
$CMAKE -S $BSSL -B $BSSL/build-android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-Wno-error" -DCMAKE_CXX_FLAGS="-Wno-error"
$CMAKE --build $BSSL/build-android-arm64 -j --target ssl crypto

# 2.2 xquic (arm64, Minsizerel)
XQ=third_party/xquic
$CMAKE -S $XQ -B $XQ/build-android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Minsizerel -DSSL_TYPE=boringssl -DSSL_PATH=$BSSL \
  -DSSL_INC_PATH=$BSSL/include \
  -DSSL_LIB_PATH=$BSSL/build-android-arm64/libssl.a\;$BSSL/build-android-arm64/libcrypto.a \
  -DXQC_ENABLE_TESTING=OFF -DXQC_BUILD_SAMPLE=OFF \
  -DXQC_ENABLE_RENO=OFF -DXQC_ENABLE_BBR2=ON -DXQC_ENABLE_COPA=OFF \
  -DXQC_ONLY_ERROR_LOG=ON -DXQC_ENABLE_TH3=ON \
  -DCMAKE_C_FLAGS="-Wno-error" -DCMAKE_CXX_FLAGS="-Wno-error"
$CMAKE --build $XQ/build-android-arm64 -j --target xquic-static

# 2.3 nghttp2 (arm64，直接编源文件)
NG=third_party/nghttp2
mkdir -p $NG/build-android-arm64/obj
cd $NG/lib
NDK_CLANG=$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android21-clang
for f in *.c; do
  $NDK_CLANG -O2 -fPIC -I$NG/lib/includes -I$NG/lib -DNGHTTP2_STATICLIB \
    -Wno-unused-parameter -c $f -o $NG/build-android-arm64/obj/${f%.c}.o
done
$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-ar rcs \
  $NG/build-android-arm64/libnghttp2.a $NG/build-android-arm64/obj/*.o

# 2.4 asternet-core + JNI 壳 (arm64)
$CMAKE -S . -B build-android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release \
  -DASTERNET_ENABLE_XQUIC=ON -DASTERNET_SHARED=ON -DASTERNET_BUILD_ANDROID=ON -DASTERNET_BUILD_TESTS=OFF \
  -DXQUIC_LIB_DIR=$XQ/build-android-arm64 \
  -DBSSL_LIB_DIR=$BSSL/build-android-arm64 \
  -DNGHTTP2_LIB_DIR=$NG/build-android-arm64
$CMAKE --build build-android-arm64 -j
# 产出：build-android-arm64/android/libasternet-jni.so
```

## 3. App 工程集成

### 3.1 build.gradle (app module)

```gradle
android {
    defaultConfig {
        minSdk 21
        ndk { abiFilters 'arm64-v8a' }
        externalNativeBuild { cmake { cppFlags '-std=c++17' } }
    }
    externalNativeBuild {
        cmake {
            path "${rootDir}/asternet/CMakeLists.txt"
            version "3.22.1"
        }
    }
}
dependencies {
    implementation "org.jetbrains.kotlinx:kotlinx-coroutines-android:1.6.4"
}
```

### 3.2 AndroidManifest.xml

```xml
<uses-permission android:name="android.permission.INTERNET" />
<application android:usesCleartextTraffic="true" ...>  <!-- POC 证书校验跳过 -->
```

### 3.3 调用示例

```kotlin
val handle = AsterNetCore.createClient()

// 切换协议测试
val r1 = AsterNetCore.requestSync(handle, "www.cloudflare.com", 443, "GET", "/",
                               AsterNetCore.Protocol.HTTP1, 12000)
println("H1: ${r1.protocolName} ${r1.status} ${r1.totalMs}ms")

val r2 = AsterNetCore.requestSync(handle, "www.cloudflare.com", 443, "GET", "/",
                               AsterNetCore.Protocol.HTTP2, 12000)
println("H2: ${r2.protocolName} ${r2.status} ${r2.totalMs}ms")

val r3 = AsterNetCore.requestSync(handle, "www.cloudflare.com", 443, "GET", "/",
                               AsterNetCore.Protocol.HTTP3, 12000)
println("H3: ${r3.protocolName} ${r3.status} ${r3.totalMs}ms")

AsterNetCore.destroyClient(handle)
```

## 4. 运行 Demo

1. 按 §2 编译 `libasternet-jni.so`（或直接用 §3.1 的 AGP externalNativeBuild 自动编译）。
2. 把 `android/src/main/java/com/AsterNet/net/` 拷入 app 工程。
3. 把 `MainActivity.kt` + `activity_main.xml` 拷入 app 工程，注册为启动 Activity。
4. 真机运行（需 arm64-v8a，POC 仅编该 ABI）。
5. 点击 HTTP/1.1 / HTTP/2 / HTTP/3 / AUTO 按钮，查看：
   - HTTP 状态码
   - 实际协议（HTTP/1.1 / HTTP/2 / HTTP/3）
   - 核心耗时（ms）
   - 墙钟耗时（ms）
   - body 预览

## 5. 协议切换说明

| protocol 值 | 行为 |
|---|---|
| `Protocol.HTTP1` (1) | 强制 HTTP/1.1，失败不降级 |
| `Protocol.HTTP2` (2) | 强制 HTTP/2（ALPN h2），失败不降级 |
| `Protocol.HTTP3` (3) | 强制 HTTP/3（QUIC），失败不降级；UDP/443 被封则失败 |
| `Protocol.AUTO` (0) | 自动降级链 H3→H2→H1.1，确保可用性 |

**测试建议**：对比同一站点 H1/H2/H3 的耗时差异（H2/H3 多路复用、H3 0-RTT 优势）；
在弱网/切网场景测 AUTO 降级链的可用性。

## 6. 注意事项

- **证书校验**：Android Demo 只导出 `AndroidCAStore` 中的系统 CA，并由 HTTP/1.1、HTTP/2、HTTP/3 加载到 BoringSSL trust store；不允许 Demo 关闭证书校验。
- **网络线程**：`requestSync` 同步阻塞，**必须在子线程调用**（Demo 已用 Thread）。
- **H3 依赖 UDP/443**：部分企业网/防火墙封 UDP/443，H3 会失败，此时用 AUTO 自动降级 H2/H1.1。
- **ABI**：POC 仅 arm64-v8a；armeabi-v7a 需额外交叉编译（参考 §2 换 ABI）。
- **0-RTT/连接迁移/连接池**：架构已留接口，当前未实现（每次请求新建连接）。

#!/bin/bash
# ================================================================
#  repack.sh — 三方库全自动管理：源码下载 → 编译 → 产出 .so
#
#  锁定版本:
#    xquic    v1.9.4   (含 BoringSSL → libssl + libcrypto)
#    nghttp2  v1.70.0
#
#  用法:
#    ./repack.sh                     # 自动下载 + 编译 + 产出 .so
#    ./repack.sh --ndk <path>        # 指定 NDK
#    ./repack.sh --clean             # 清空源码和产物，重新来
# ================================================================
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ---- 版本 ----
XQUIC_VERSION="v1.9.4"
NGHTTP2_VERSION="v1.70.0"
XQUIC_URL="https://github.com/alibaba/xquic"
NGHTTP2_URL="https://github.com/nghttp2/nghttp2"

# ---- 路径 ----
XQUIC_SRC="$SCRIPT_DIR/xquic"
NGHTTP2_SRC="$SCRIPT_DIR/nghttp2"
BSSL_SRC="$XQUIC_SRC/third_party/boringssl"
OUT_DIR="$SCRIPT_DIR/libs/arm64-v8a"
NDK=""
DO_CLEAN=false

# ---- 颜色 ----
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
red()    { printf '\033[31m%s\033[0m\n' "$*"; }

# ---- 参数 ----
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) DO_CLEAN=true; shift;;
        --ndk) NDK="$2"; shift 2;;
        -h|--help)
            echo "Usage: $0 [--clean] [--ndk <path>]"
            echo "  Auto-download source, build, produce .so"
            echo "  Locked: xquic=$XQUIC_VERSION  nghttp2=$NGHTTP2_VERSION"
            exit 0
            ;;
        *) shift;;
    esac
done

# ---- NDK ----
find_ndk() {
    for d in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK:-}" "$HOME/Library/Android/sdk/ndk"/*; do
        [ -d "$d" ] && NDK="$d" && return
    done
}
{ [ -z "${NDK:-}" ] || [ ! -d "${NDK:-}" ]; } && find_ndk
[ -z "${NDK:-}" ] && { red "NDK not found"; exit 1; }

TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
[ ! -f "$TOOLCHAIN_FILE" ] && { red "toolchain not found: $TOOLCHAIN_FILE"; exit 1; }

# cmake binary from SDK
CMAKE_BIN=""
for d in "$HOME/Library/Android/sdk/cmake"/*; do
    [ -f "$d/bin/cmake" ] && CMAKE_BIN="$d/bin/cmake" && break
done
[ -z "${CMAKE_BIN:-}" ] && CMAKE_BIN="cmake"

# NDK compiler (for .a → .so repack)
TOOLCHAIN_DIR="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
for h in "darwin-x86_64" "linux-x86_64" "windows-x86_64"; do
    [ -d "$NDK/toolchains/llvm/prebuilt/$h" ] && TOOLCHAIN_DIR="$NDK/toolchains/llvm/prebuilt/$h" && break
done
CC="$(ls "$TOOLCHAIN_DIR/bin/aarch64-linux-android"*clang++ 2>/dev/null | head -1)"
[ -z "${CC:-}" ] && CC="$(ls "$TOOLCHAIN_DIR/bin/aarch64-linux-android"*clang 2>/dev/null | head -1)"
STRIP="$(ls "$TOOLCHAIN_DIR/bin/llvm-strip" 2>/dev/null | head -1)" || true

if $DO_CLEAN; then
    rm -rf "$XQUIC_SRC" "$NGHTTP2_SRC" "$SCRIPT_DIR/build-"* "$OUT_DIR"
fi

# ================================================================
#  源码下载
# ================================================================

clone_or_update() {
    local url="$1" dir="$2" version="$3" label="$4"

    # 已有 git repo → 只切换版本，不删除
    if [ -d "$dir/.git" ]; then
        local current
        current=$(cd "$dir" && git describe --tags 2>/dev/null || echo "unknown")
        case "$current" in
            *"$version"*)
                green "  $label: $current ✓" ;;
            *)
                yellow "  $label: $current → $version"
                (cd "$dir" && git fetch --tags --depth 1 2>/dev/null || git fetch --tags 2>/dev/null) || true
                (cd "$dir" && git checkout "$version" 2>/dev/null) || true
                (cd "$dir" && git submodule update --init --recursive --depth 1 2>/dev/null) || true
                green "  $label: 已切换到 $version" ;;
        esac
        return
    fi

    # 有预编译产物的非 git 目录 → 不覆盖（如 symlink 指向的 third_party/xquic）
    if [ -d "$dir" ] && [ ! -d "$dir/.git" ]; then
        yellow "  $label: 使用已有目录（非 git），跳过下载"
        return
    fi

    # 目录不存在 → 从 GitHub 下载
    echo "  $label: 从 GitHub 下载 $version ..."
    git clone --branch "$version" --depth 1 "$url" "$dir" 2>/dev/null || {
        yellow "  $label: shallow clone 失败，尝试完整 clone ..."
        git clone "$url" "$dir" 2>/dev/null || { red "  ✗ clone 失败"; return 1; }
        (cd "$dir" && git checkout "$version" 2>/dev/null) || true
    }
    if [ -f "$dir/.gitmodules" ]; then
        (cd "$dir" && git submodule update --init --recursive --depth 1 2>/dev/null) || true
    fi
    green "  $label: ✓ $version"
}

download_sources() {
    echo ""
    echo "=== 源码下载 ==="
    clone_or_update "$XQUIC_URL"   "$XQUIC_SRC"   "$XQUIC_VERSION"   "xquic"
    clone_or_update "$NGHTTP2_URL" "$NGHTTP2_SRC" "$NGHTTP2_VERSION" "nghttp2"
}

# ================================================================
#  编译
# ================================================================

cmake_build() {
    local src="$1" build_dir="$2" label="$3"
    shift 3

    echo "  $label: cmake configure ..."
    rm -rf "$build_dir"
    mkdir -p "$build_dir"

    "$CMAKE_BIN" -S "$src" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-21 \
        -DCMAKE_BUILD_TYPE=Release \
        -GNinja \
        "$@" 2>&1 | tail -1

    echo "  $label: cmake build ..."
    "$CMAKE_BIN" --build "$build_dir" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" 2>&1 | tail -1

    green "  $label: ✓"
}

build_all_from_source() {
    echo ""
    echo "=== 从源码编译 ==="

    # 1. BoringSSL → libcrypto.so + libssl.so
    if [ ! -f "$OUT_DIR/libcrypto.so" ] || [ ! -f "$OUT_DIR/libssl.so" ]; then
        cmake_build "$BSSL_SRC" "$SCRIPT_DIR/build-bssl" "boringssl" \
            -DBUILD_SHARED_LIBS=ON

        local bssl_out="$SCRIPT_DIR/build-bssl"
        [ -f "$bssl_out/crypto/libcrypto.so" ] && cp "$bssl_out/crypto/libcrypto.so" "$OUT_DIR/"
        [ -f "$bssl_out/ssl/libssl.so" ]       && cp "$bssl_out/ssl/libssl.so"       "$OUT_DIR/"
        # fallback: search
        find "$bssl_out" -name "libcrypto.so" -exec cp {} "$OUT_DIR/" \; 2>/dev/null || true
        find "$bssl_out" -name "libssl.so"    -exec cp {} "$OUT_DIR/" \; 2>/dev/null || true
    else
        green "  boringssl: .so 已存在，跳过"
    fi

    # 2. nghttp2 → libnghttp2.so
    if [ ! -f "$OUT_DIR/libnghttp2.so" ]; then
        cmake_build "$NGHTTP2_SRC" "$SCRIPT_DIR/build-nghttp2" "nghttp2" \
            -DENABLE_LIB_ONLY=ON \
            -DENABLE_SHARED_LIB=ON \
            -DENABLE_STATIC_LIB=OFF

        local ng_out="$SCRIPT_DIR/build-nghttp2/lib/libnghttp2.so"
        [ -f "$ng_out" ] && cp "$ng_out" "$OUT_DIR/"
        find "$SCRIPT_DIR/build-nghttp2" -name "libnghttp2.so" -exec cp {} "$OUT_DIR/" \; 2>/dev/null || true
    else
        green "  nghttp2: .so 已存在，跳过"
    fi

    # 3. xquic → libxquic.so
    if [ ! -f "$OUT_DIR/libxquic.so" ]; then
        cmake_build "$XQUIC_SRC" "$SCRIPT_DIR/build-xquic" "xquic" \
            -DBUILD_SHARED_LIBS=ON \
            -DBORINGSSL_DIR="$BSSL_SRC"

        local xq_out="$SCRIPT_DIR/build-xquic/lib/libxquic.so"
        [ -f "$xq_out" ] && cp "$xq_out" "$OUT_DIR/"
        find "$SCRIPT_DIR/build-xquic" -name "libxquic.so" -exec cp {} "$OUT_DIR/" \; 2>/dev/null || true
    else
        green "  xquic: .so 已存在，跳过"
    fi
}

# ================================================================
#  回退方案：prebuilt .a → .so repack
# ================================================================

repack_a_to_so() {
    local in="$1" out="$2" label="$3" deps="$4"
    if [ ! -f "$in" ]; then return 1; fi

    echo "  $label: .a → .so repack ..."
    "$CC" -shared -o "$out" \
        -Wl,--whole-archive "$in" -Wl,--no-whole-archive \
        -L"$OUT_DIR" $deps \
        -Wl,-soname,"$(basename "$out")" \
        -fPIC -O3 -s 2>/dev/null
    [ -n "${STRIP:-}" ] && "$STRIP" --strip-debug "$out" 2>/dev/null || true
    return 0
}

fallback_repack() {
    local CRYPTO_A="$XQUIC_SRC/third_party/boringssl/build-android-arm64/libcrypto.a"
    local SSL_A="$XQUIC_SRC/third_party/boringssl/build-android-arm64/libssl.a"
    local XQUIC_A="$XQUIC_SRC/build-android-arm64/libxquic-static.a"

    if [ -f "$CRYPTO_A" ] && [ -f "$SSL_A" ] && [ -f "$XQUIC_A" ]; then
        echo ""
        yellow "=== 使用预编译 .a（跳过源码编译）==="
        repack_a_to_so "$CRYPTO_A" "$OUT_DIR/libcrypto.so" "libcrypto" ""
        repack_a_to_so "$SSL_A"    "$OUT_DIR/libssl.so"    "libssl"    "-lcrypto"
        repack_a_to_so "$XQUIC_A"  "$OUT_DIR/libxquic.so"  "libxquic"  "-lssl -lcrypto"
        return 0
    fi
    return 1
}

# ================================================================
#  主流程
# ================================================================

main() {
    echo ""
    echo "  AsterNet 三方库 — 全自动"
    echo "  xquic $XQUIC_VERSION    nghttp2 $NGHTTP2_VERSION"
    echo ""

    mkdir -p "$OUT_DIR"

    download_sources

    # 优先用预编译 .a 快速 repack（已有编译产物时）
    if fallback_repack; then
        # nghttp2 仍需从源码编译（无 prebuilt .a 或不匹配版本）
        if [ ! -f "$OUT_DIR/libnghttp2.so" ]; then
            cmake_build "$NGHTTP2_SRC" "$SCRIPT_DIR/build-nghttp2" "nghttp2" \
                -DENABLE_LIB_ONLY=ON -DENABLE_SHARED_LIB=ON -DENABLE_STATIC_LIB=OFF
            local ng="$SCRIPT_DIR/build-nghttp2/lib/libnghttp2.so"
            [ -f "$ng" ] && cp "$ng" "$OUT_DIR/"
            find "$SCRIPT_DIR/build-nghttp2" -name "libnghttp2.so" -exec cp {} "$OUT_DIR/" \; 2>/dev/null || true
        fi
    else
        # 无预编译 .a → 全部从源码编译
        build_all_from_source
    fi

    echo ""
    green "=== 产物: $OUT_DIR ==="
    for so in libcrypto.so libssl.so libnghttp2.so libxquic.so; do
        if [ -f "$OUT_DIR/$so" ]; then
            green "  ✓ $so  ($(du -sh "$OUT_DIR/$so" | cut -f1))"
        else
            red "  ✗ $so  缺失"
        fi
    done
    echo ""
    green "  APK 构建就绪: ./gradlew :examples:android:assembleDebug"
}

main

#!/bin/bash
# ================================================================
#  repack.sh — 一键：源码下载 → 编译 → 独立 .so
#
#  ./repack.sh  首次 ~10分钟，之后秒级完成
#
#  锁定版本: xquic v1.9.4   nghttp2 v1.70.0
# ================================================================
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

XQUIC_VERSION="v1.9.4"
NGHTTP2_VERSION="v1.70.0"
XQUIC_URL="https://github.com/alibaba/xquic"
NGHTTP2_URL="https://github.com/nghttp2/nghttp2"

XQUIC_SRC="$SCRIPT_DIR/xquic"
NGHTTP2_SRC="$SCRIPT_DIR/nghttp2"
BSSL_SRC="$XQUIC_SRC/third_party/boringssl"
OUT_DIR="$SCRIPT_DIR/libs/arm64-v8a"
ABI="arm64-v8a"

green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }
red()    { printf '\033[31m%s\033[0m\n' "$*"; }

# ---- NDK + CMake ----
find_ndk() {
    for d in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK:-}" "$HOME/Library/Android/sdk/ndk"/*; do
        [ -d "$d" ] && { NDK="$d"; return; }
    done
}
NDK=""; find_ndk
[ -z "${NDK:-}" ] && { red "NDK 未找到，请设置 ANDROID_NDK_HOME"; exit 1; }

TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"

# 优先用新版 CMake（BoringSSL 需要 ≥3.22）
CMAKE=""
for d in "$HOME/Library/Android/sdk/cmake/4.0.2" "$HOME/Library/Android/sdk/cmake/3.22.1"; do
    [ -f "$d/bin/cmake" ] && CMAKE="$d/bin/cmake" && break
done
[ -z "$CMAKE" ] && CMAKE="cmake"
echo "  CMake: $CMAKE"

# ---- 源码下载 ----
clone_source() {
    local url="$1" dir="$2" version="$3" label="$4"
    if [ -d "$dir/.git" ]; then
        local cur; cur=$(cd "$dir" && git describe --tags 2>/dev/null || echo "unknown")
        case "$cur" in *"$version"*) green "  $label: $version ✓"; return ;; esac
        yellow "  $label: $cur → $version"
        (cd "$dir" && git fetch --tags 2>/dev/null && git checkout "$version" 2>/dev/null) || true
        return
    fi
    [ -d "$dir" ] && rm -rf "$dir"
    echo "  $label: git clone $version ..."
    git clone --branch "$version" --depth 1 "$url" "$dir" 2>/dev/null || {
        git clone "$url" "$dir" 2>/dev/null || { red "  clone 失败"; return 1; }
        (cd "$dir" && git checkout "$version" 2>/dev/null) || true
    }
    green "  $label: ✓"
}

download_all() {
    echo ""
    echo "=== 源码 ==="
    clone_source "$XQUIC_URL"   "$XQUIC_SRC"   "$XQUIC_VERSION"   "xquic"
    clone_source "$NGHTTP2_URL" "$NGHTTP2_SRC" "$NGHTTP2_VERSION" "nghttp2"

    # BoringSSL：xquic v1.9.4 没有用 git submodule 管理，需单独放置
    if [ ! -f "$BSSL_SRC/CMakeLists.txt" ]; then
        echo "  boringssl: git clone ..."
        rm -rf "$BSSL_SRC"
        git clone --depth 1 https://github.com/google/boringssl "$BSSL_SRC" 2>/dev/null || {
            red "  boringssl: clone 失败，请手动 git clone 到 $BSSL_SRC"; return 1;
        }
    fi
    green "  boringssl: ✓"
}

# ---- 编译 ----
run_cmake() {
    local src="$1" build="$2" label="$3"; shift 3
    echo "  $label: cmake configure ..."
    rm -rf "$build"; mkdir -p "$build"

    local log="$SCRIPT_DIR/build-$label.log"
    if "$CMAKE" -S "$src" -B "$build" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-21" \
        -DCMAKE_BUILD_TYPE=Release -GNinja "$@" >"$log" 2>&1; then
        echo "    configure OK"
    else
        red "    configure FAILED — 查看: $log"
        tail -30 "$log"
        return 1
    fi

    echo "  $label: cmake build ..."
    if "$CMAKE" --build "$build" -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >>"$log" 2>&1; then
        green "  $label: ✓"
    else
        red "    build FAILED — 查看: $log"
        tail -30 "$log"
        return 1
    fi
}

build_all() {
    echo ""
    echo "=== 编译 ==="

    # 1. BoringSSL
    local bssl_out="$BSSL_SRC/build-android-$ABI"
    if [ ! -f "$bssl_out/libcrypto.a" ]; then
        run_cmake "$BSSL_SRC" "$bssl_out" "boringssl" \
            -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
        [ ! -f "$bssl_out/libcrypto.a" ] && { red "  libcrypto.a 未产出"; return 1; }
    else
        green "  boringssl: ✓"
    fi

    # 2. xquic
    local xq_out="$XQUIC_SRC/build-android-$ABI"
    if [ ! -f "$xq_out/libxquic-static.a" ]; then
        run_cmake "$XQUIC_SRC" "$xq_out" "xquic" \
            -DBUILD_SHARED_LIBS=OFF \
            -DSSL_TYPE=boringssl \
            -DSSL_PATH="$BSSL_SRC" \
            -DSSL_INCLUDE_DIR="$BSSL_SRC/include" \
            -DSSL_LIBRARY_STATIC="$bssl_out/libssl.a" \
            -DCRYPTO_LIBRARY_STATIC="$bssl_out/libcrypto.a"
        [ ! -f "$xq_out/libxquic-static.a" ] && { red "  libxquic-static.a 未产出"; return 1; }
    else
        green "  xquic: ✓"
    fi

    # 3. nghttp2
    local ng_out="$NGHTTP2_SRC/build-android-$ABI"
    if [ ! -f "$ng_out/lib/libnghttp2.a" ]; then
        run_cmake "$NGHTTP2_SRC" "$ng_out" "nghttp2" \
            -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON \
            -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
        [ ! -f "$ng_out/lib/libnghttp2.a" ] && { red "  libnghttp2.a 未产出"; return 1; }
    else
        green "  nghttp2: ✓"
    fi
}

# ---- .a → .so ----
repack() {
    local in="$1" so="$2" label="$3" deps="$4"
    [ ! -f "$in" ] && { red "  ✗ $label: $in 缺失"; return 1; }

    local dir="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
    for h in darwin-x86_64 linux-x86_64; do
        [ -d "$NDK/toolchains/llvm/prebuilt/$h" ] && dir="$NDK/toolchains/llvm/prebuilt/$h" && break
    done
    local cc; cc=$(ls "$dir/bin/aarch64-linux-android"*clang++ 2>/dev/null | head -1)
    [ -z "${cc:-}" ] && cc=$(ls "$dir/bin/aarch64-linux-android"*clang 2>/dev/null | head -1)

    echo "  $label ..."
    "$cc" -shared -o "$so" \
        -Wl,--whole-archive "$in" -Wl,--no-whole-archive \
        -L"$OUT_DIR" $deps -Wl,-soname,"$(basename "$so")" \
        -fPIC -O3 -s 2>/dev/null

    local strip; strip=$(ls "$dir/bin/llvm-strip" 2>/dev/null | head -1) || true
    [ -n "${strip:-}" ] && "$strip" --strip-debug "$so" 2>/dev/null || true
    green "    → $(du -sh "$so" | cut -f1)"
}

# ---- 主流程 ----
main() {
    echo ""
    echo "  AsterNet 三方库"
    echo "  xquic $XQUIC_VERSION   nghttp2 $NGHTTP2_VERSION"
    echo ""

    download_all || exit 1
    build_all || exit 1

    echo ""
    echo "=== .a → .so ==="
    rm -rf "$OUT_DIR"; mkdir -p "$OUT_DIR"

    local bssl_out="$BSSL_SRC/build-android-$ABI"
    local xq_out="$XQUIC_SRC/build-android-$ABI"
    local ng_out="$NGHTTP2_SRC/build-android-$ABI"

    repack "$bssl_out/libcrypto.a"      "$OUT_DIR/libcrypto.so"  "libcrypto"  ""
    repack "$bssl_out/libssl.a"         "$OUT_DIR/libssl.so"     "libssl"     "-lcrypto"
    repack "$ng_out/lib/libnghttp2.a"   "$OUT_DIR/libnghttp2.so" "libnghttp2" "-lssl -lcrypto"
    repack "$xq_out/libxquic-static.a"  "$OUT_DIR/libxquic.so"   "libxquic"   "-lssl -lcrypto"

    echo ""
    green "=== 产物 ==="
    ls -la "$OUT_DIR/"
    echo ""
    green "  ./gradlew :examples:android:assembleDebug"
}

main

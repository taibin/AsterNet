#!/bin/bash
# ================================================================
#  AsterNet 统一构建脚本
#
#  用法:
#    ./scripts/build.sh --all              # 全平台构建
#    ./scripts/build.sh --android          # 仅 Android
#    ./scripts/build.sh --android --debug  # Android Debug（可调试）
#    ./scripts/build.sh --cxx              # C++ 核心 + 测试
#    ./scripts/build.sh --cxx --test       # 编译并运行 C++ 测试
# ================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORM=""
MODE="release"
RUN_TESTS=false

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

usage() {
    echo "Usage: $0 [--all|--android|--cxx|--ios] [--debug] [--test]"
    echo ""
    echo "  --all        Build all platforms"
    echo "  --android    Build Android SDK + Example APK"
    echo "  --cxx        Build C++ core + tests (desktop)"
    echo "  --ios        Build iOS (placeholder)"
    echo "  --debug      Debug build (default: release)"
    echo "  --test       Run tests after build (cxx only)"
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --all)     PLATFORM="all" ;;
        --android) PLATFORM="android" ;;
        --cxx)     PLATFORM="cxx" ;;
        --ios)     PLATFORM="ios" ;;
        --debug)   MODE="debug" ;;
        --test)    RUN_TESTS=true ;;
        -h|--help) usage ;;
        *) red "Unknown option: $1"; usage ;;
    esac
    shift
done

if [ -z "$PLATFORM" ]; then
    red "请指定构建平台: --all | --android | --cxx | --ios"
    usage
fi

cd "$ROOT"

# ---- 确保三方库存在 ----
ensure_third_party() {
    if [ ! -f "sdk/third_party/nghttp2/build-android-arm64/libnghttp2.a" ]; then
        yellow "三方库未找到，尝试下载..."
        if [ -x "scripts/setup-third-party.sh" ]; then
            ./scripts/setup-third-party.sh --local sdk/third_party/xquic sdk/third_party/nghttp2 2>/dev/null || true
        fi
    fi
}

build_cxx() {
    local build_type="Release"
    [ "$MODE" = "debug" ] && build_type="Debug"

    green "=== 构建 C++ 核心 + 测试 ($build_type) ==="
    cmake -S . -B build/cxx \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DASTERNET_BUILD_TESTS=ON \
        -DASTERNET_BUILD_ANDROID=OFF \
        -DASTERNET_BUILD_IOS=OFF \
        -DASTERNET_ENABLE_XQUIC=OFF
    cmake --build build/cxx -j

    if [ "$RUN_TESTS" = true ]; then
        green "=== 运行 C++ 单元测试 ==="
        cd build/cxx && ctest --output-on-failure && cd "$ROOT"
    fi
}

build_android() {
    local task="assembleDebug"
    [ "$MODE" = "release" ] && task="assembleRelease"

    ensure_third_party

    green "=== 构建 Android SDK + Example ($task) ==="
    ./gradlew ":examples:android:$task" --no-daemon

    local apk="examples/android/build/outputs/apk/${MODE}/examples-android-${MODE}.apk"
    if [ -f "$apk" ]; then
        green "=== APK 产物: $apk ==="
    fi
}

build_ios() {
    yellow "=== iOS 构建（当前为占位，待实现）==="
}

build_all() {
    build_cxx
    build_android
    build_ios
}

echo ""
echo "  AsterNet Build"
echo "  Platform: $PLATFORM  Mode: $MODE"
echo ""

case "$PLATFORM" in
    all)      build_all ;;
    android)  build_android ;;
    cxx)      build_cxx ;;
    ios)      build_ios ;;
esac

green "构建完成"

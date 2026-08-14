#!/bin/bash
# ================================================================
#  AsterNet 第三方依赖下载脚本
#
#  用途：下载 nghttp2 / xquic / boringssl 的预编译静态库 (.a)
#        以及头文件，放置到 CMake 期望的路径。
#
#  用法：
#    ./setup-third-party.sh                    # 从默认 URL 下载
#    ./setup-third-party.sh --local <xquic_dir> [nghttp2_dir]
#    ./setup-third-party.sh --url <base_url>
# ================================================================

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRD="$ROOT/sdk/third_party"
ABI="arm64-v8a"
BUILD_DIR="build-android-$ABI"
BASE_URL="https://github.com/YOUR_ORG/asternet/releases/download/libs-v1"

# ---- 颜色 ----
red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

# ---- 参数 ----
MODE="download"
LOCAL_XQUIC=""
LOCAL_NGHTTP2=""

while [ $# -gt 0 ]; do
    case "$1" in
        --local)
            MODE="local"
            LOCAL_XQUIC="$2"
            LOCAL_NGHTTP2="${3:-}"
            shift 2
            [ -n "$LOCAL_NGHTTP2" ] && shift
            ;;
        --url)
            BASE_URL="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--local <xquic-dir> [nghttp2-dir]] [--url <base-url>]"
            exit 0
            ;;
        *)
            red "Unknown option: $1"
            exit 1
            ;;
    esac
done

# ---- 工具函数 ----
ensure_dir() { mkdir -p "$(dirname "$1")"; }

download_one() {
    local url="$1" dest="$2" label="$3"
    ensure_dir "$dest"
    if command -v curl >/dev/null 2>&1; then
        curl -fSL --progress-bar -o "$dest" "$url" || { red "[FAIL] $label"; return 1; }
    elif command -v wget >/dev/null 2>&1; then
        wget -q --show-progress -O "$dest" "$url" || { red "[FAIL] $label"; return 1; }
    else
        red "需要 curl 或 wget"; exit 1
    fi
    green "[ OK ] $label"
}

copy_one() {
    local src="$1" dest="$2" label="$3"
    if [ ! -f "$src" ] && [ ! -d "$src" ]; then
        yellow "[SKIP] $label — 未找到 $src"
        return 0
    fi
    # 同文件跳过（macOS cp 会报错）
    if [ -f "$dest" ] && cmp -s "$src" "$dest" 2>/dev/null; then
        green "[ OK ] $label (unchanged)"
        return 0
    fi
    ensure_dir "$dest"
    cp -R "$src" "$dest"
    green "[ OK ] $label (local)"
}

extract_tar() {
    local archive="$1" dest_dir="$2" label="$3"
    if [ ! -f "$archive" ]; then
        yellow "[SKIP] $label — 未找到"
        return 0
    fi
    ensure_dir "$dest_dir"
    tar -xzf "$archive" -C "$dest_dir"
    green "[ OK ] $label"
}

# ---- 校验 ----
verify_all() {
    echo ""
    echo "── 校验第三方依赖 ──"
    local miss=0

    check() {
        local p="$THIRD/$1"
        if [ -f "$p" ] || [ -d "$p" ]; then
            local sz; sz=$(du -sh "$p" 2>/dev/null | cut -f1)
            green "  [✓] $1  ($sz)"
        else
            red "  [✗] $1  缺失"
            miss=1
        fi
    }

    check "xquic/include"
    check "xquic/third_party/boringssl/include"
    check "xquic/$BUILD_DIR/libxquic-static.a"
    check "xquic/third_party/boringssl/$BUILD_DIR/libssl.a"
    check "xquic/third_party/boringssl/$BUILD_DIR/libcrypto.a"
    check "nghttp2/$BUILD_DIR/lib/libnghttp2.a"
    check "nghttp2/lib/includes"

    if [ "$miss" -eq 0 ]; then
        echo ""
        green "═══════════════════════════════════════"
        green "  第三方依赖就绪"
        green "  ./gradlew :examples:android:assembleDebug"
        green "═══════════════════════════════════════"
    else
        echo ""
        red "部分文件缺失，请检查后重试。"
        return 1
    fi
}

# ---- 主流程 ----
echo ""
echo "  AsterNet — 第三方依赖下载"
echo "  目标: $ABI"
echo ""

if [ "$MODE" = "local" ]; then
    echo "  模式: 本地拷贝 ← $LOCAL_XQUIC"
    echo ""

    copy_one "$LOCAL_XQUIC/include"                       "$THIRD/xquic/include"                        "xquic headers"
    copy_one "$LOCAL_XQUIC/third_party/boringssl/include" "$THIRD/xquic/third_party/boringssl/include"  "boringssl headers"
    copy_one "$LOCAL_XQUIC/$BUILD_DIR/libxquic-static.a" \
             "$THIRD/xquic/$BUILD_DIR/libxquic-static.a"   "libxquic-static.a"
    copy_one "$LOCAL_XQUIC/third_party/boringssl/$BUILD_DIR/libssl.a" \
             "$THIRD/xquic/third_party/boringssl/$BUILD_DIR/libssl.a"   "libssl.a"
    copy_one "$LOCAL_XQUIC/third_party/boringssl/$BUILD_DIR/libcrypto.a" \
             "$THIRD/xquic/third_party/boringssl/$BUILD_DIR/libcrypto.a" "libcrypto.a"

    if [ -n "$LOCAL_NGHTTP2" ]; then
        copy_one "$LOCAL_NGHTTP2/$BUILD_DIR/lib/libnghttp2.a" \
                 "$THIRD/nghttp2/$BUILD_DIR/lib/libnghttp2.a"  "libnghttp2.a"
    fi

    verify_all

else
    echo "  模式: 远程下载 ← $BASE_URL"
    echo ""

    # 头文件
    download_one "$BASE_URL/xquic-headers-${ABI}.tar.gz" \
                 "$THIRD/xquic/include.tar.gz"            "xquic headers archive"
    download_one "$BASE_URL/boringssl-headers-${ABI}.tar.gz" \
                 "$THIRD/xquic/third_party/boringssl/include.tar.gz" "boringssl headers archive"

    extract_tar "$THIRD/xquic/include.tar.gz"                               "$THIRD/xquic"                        "xquic headers"
    extract_tar "$THIRD/xquic/third_party/boringssl/include.tar.gz"          "$THIRD/xquic/third_party/boringssl" "boringssl headers"
    rm -f "$THIRD/xquic/include.tar.gz" "$THIRD/xquic/third_party/boringssl/include.tar.gz"

    # 静态库
    download_one "$BASE_URL/libxquic-static-${ABI}.a" \
                 "$THIRD/xquic/$BUILD_DIR/libxquic-static.a"   "libxquic-static.a"
    download_one "$BASE_URL/libssl-${ABI}.a" \
                 "$THIRD/xquic/third_party/boringssl/$BUILD_DIR/libssl.a"  "libssl.a"
    download_one "$BASE_URL/libcrypto-${ABI}.a" \
                 "$THIRD/xquic/third_party/boringssl/$BUILD_DIR/libcrypto.a" "libcrypto.a"
    download_one "$BASE_URL/libnghttp2-${ABI}.a" \
                 "$THIRD/nghttp2/$BUILD_DIR/lib/libnghttp2.a"       "libnghttp2.a"

    verify_all
fi

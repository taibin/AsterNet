#!/bin/bash
# ================================================================
#  AsterNet 第三方依赖打包脚本
#
#  用途：将本地已编译的 .a 文件和头文件打包，准备上传到
#        GitHub Releases / CDN，供 setup-third-party.sh 下载。
#
#  用法：
#    ./package-third-party.sh              # 打包 arm64-v8a
#    ./package-third-party.sh --all-abi     # 打包所有 ABI
#
#  输出：third_party_release/ 目录，包含所有 tar.gz 文件
# ================================================================

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRD="$ROOT/sdk/third_party"
OUT="$ROOT/third_party_release"
VERSION="${1:-v1}"

rm -rf "$OUT"
mkdir -p "$OUT"

ABIS=("arm64-v8a")
if [[ "${1:-}" == "--all-abi" ]]; then
    ABIS=("arm64-v8a" "armeabi-v7a" "x86_64" "x86")
fi

green() { echo -e "\033[32m$*\033[0m"; }
red()   { echo -e "\033[31m$*\033[0m"; }

echo "打包第三方依赖 (ABI: ${ABIS[*]}) ..."
echo ""

for abi in "${ABIS[@]}"; do

    # ---- xquic 头文件 ----
    if [[ -d "$THIRD/xquic/include" ]]; then
        tar -czf "$OUT/xquic-headers-${abi}.tar.gz" \
            -C "$THIRD/xquic" include/
        green "  ✓  xquic-headers-${abi}.tar.gz"
    else
        red "  ✗  third_party/xquic/include/ 不存在，跳过"
    fi

    # ---- boringssl 头文件 ----
    if [[ -d "$THIRD/xquic/third_party/boringssl/include" ]]; then
        tar -czf "$OUT/boringssl-headers-${abi}.tar.gz" \
            -C "$THIRD/xquic/third_party/boringssl" include/
        green "  ✓  boringssl-headers-${abi}.tar.gz"
    else
        red "  ✗  third_party/xquic/third_party/boringssl/include/ 不存在"
    fi

    # ---- 静态库 ----
    pack_lib() {
        local src="$1"
        local name="$2"
        if [[ -f "$src" ]]; then
            cp "$src" "$OUT/${name}-${abi}.a"
            green "  ✓  ${name}-${abi}.a ($(du -sh "$src" | cut -f1))"
        else
            red "  ✗  $src 不存在，跳过"
        fi
    }

    pack_lib "$THIRD/xquic/build-android-${abi}/libxquic-static.a" \
             "libxquic-static"

    pack_lib "$THIRD/xquic/third_party/boringssl/build-android-${abi}/libssl.a" \
             "libssl"

    pack_lib "$THIRD/xquic/third_party/boringssl/build-android-${abi}/libcrypto.a" \
             "libcrypto"

    pack_lib "$THIRD/nghttp2/build-android-${abi}/libnghttp2.a" \
             "libnghttp2"

done

echo ""
echo "═══════════════════════════════════════"
echo "  输出目录: $OUT"
echo ""
echo "  上传到 GitHub Releases："
echo "    gh release create libs-${VERSION} $OUT/* \\"
echo "      --title 'Prebuilt libs ${VERSION}' \\"
echo "      --notes '预编译第三方静态库 (ABI: ${ABIS[*]})'"
echo "═══════════════════════════════════════"

# AsterNet 三方库构建
# 将预编译的 .a 打包为独立 .so（动态库），不再静态链入 libasternet.so。
#
# 产物：
#   libnghttp2.so   — HTTP/2 协议
#   libssl.so       — TLS（boringssl）
#   libcrypto.so    — 加密（boringssl）
#   libxquic.so     — QUIC / HTTP/3
#
# 用法（Android NDK）：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21
#   cmake --build build
#
cmake_minimum_required(VERSION 3.18)
project(asternet-third-party C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- nghttp2 → libnghttp2.so ----
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nghttp2/build-android-${ANDROID_ABI}/libnghttp2.a")
    add_library(nghttp2 SHARED IMPORTED GLOBAL)
    set_target_properties(nghttp2 PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/nghttp2/build-android-${ANDROID_ABI}/libnghttp2.so")
    message(STATUS "nghttp2: using prebuilt .so")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nghttp2/build-android-${ANDROID_ABI}/libnghttp2.a")
    # 从 .a 解包并重新打包为 .so
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libnghttp2.so"
        COMMAND ${CMAKE_CXX_COMPILER} -shared -o "${CMAKE_CURRENT_BINARY_DIR}/libnghttp2.so"
                -Wl,--whole-archive
                "${CMAKE_CURRENT_SOURCE_DIR}/nghttp2/build-android-${ANDROID_ABI}/libnghttp2.a"
                -Wl,--no-whole-archive
        COMMENT "Repacking libnghttp2.a → libnghttp2.so"
    )
    add_library(nghttp2 SHARED IMPORTED GLOBAL)
    set_target_properties(nghttp2 PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libnghttp2.so")
    add_dependencies(nghttp2 "${CMAKE_CURRENT_BINARY_DIR}/libnghttp2.so")
endif()

# ---- BoringSSL → libssl.so + libcrypto.so ----
set(BSSL_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xquic/third_party/boringssl/build-android-${ANDROID_ABI}")
if(EXISTS "${BSSL_BUILD_DIR}/libssl.so")
    add_library(bssl_ssl SHARED IMPORTED GLOBAL)
    set_target_properties(bssl_ssl PROPERTIES IMPORTED_LOCATION "${BSSL_BUILD_DIR}/libssl.so")
    add_library(bssl_crypto SHARED IMPORTED GLOBAL)
    set_target_properties(bssl_crypto PROPERTIES IMPORTED_LOCATION "${BSSL_BUILD_DIR}/libcrypto.so")
elseif(EXISTS "${BSSL_BUILD_DIR}/libssl.a")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libssl.so"
        COMMAND ${CMAKE_CXX_COMPILER} -shared -o "${CMAKE_CURRENT_BINARY_DIR}/libssl.so"
                -Wl,--whole-archive "${BSSL_BUILD_DIR}/libssl.a" -Wl,--no-whole-archive
                "${BSSL_BUILD_DIR}/libcrypto.a"
        COMMENT "Repacking libssl.a + libcrypto.a → libssl.so"
    )
    add_library(bssl_ssl SHARED IMPORTED GLOBAL)
    set_target_properties(bssl_ssl PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libssl.so")
    add_dependencies(bssl_ssl "${CMAKE_CURRENT_BINARY_DIR}/libssl.so")
    add_library(bssl_crypto SHARED IMPORTED GLOBAL)
    set_target_properties(bssl_crypto PROPERTIES IMPORTED_LOCATION "${BSSL_BUILD_DIR}/libcrypto.a")
endif()

# ---- xquic → libxquic.so ----
set(XQUIC_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xquic/build-android-${ANDROID_ABI}")
if(EXISTS "${XQUIC_BUILD_DIR}/libxquic.so")
    add_library(xquic SHARED IMPORTED GLOBAL)
    set_target_properties(xquic PROPERTIES IMPORTED_LOCATION "${XQUIC_BUILD_DIR}/libxquic.so")
elseif(EXISTS "${XQUIC_BUILD_DIR}/libxquic-static.a")
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/libxquic.so"
        COMMAND ${CMAKE_CXX_COMPILER} -shared -o "${CMAKE_CURRENT_BINARY_DIR}/libxquic.so"
                -Wl,--whole-archive "${XQUIC_BUILD_DIR}/libxquic-static.a" -Wl,--no-whole-archive
        COMMENT "Repacking libxquic-static.a → libxquic.so"
    )
    add_library(xquic SHARED IMPORTED GLOBAL)
    set_target_properties(xquic PROPERTIES IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/libxquic.so")
    add_dependencies(xquic "${CMAKE_CURRENT_BINARY_DIR}/libxquic.so")
endif()

# 安装目标
install(TARGETS nghttp2 bssl_ssl bssl_crypto xquic
    LIBRARY DESTINATION lib/${ANDROID_ABI} OPTIONAL)

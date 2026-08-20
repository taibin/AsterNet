# AsterNet Android Example CMake
# 编译 libasternet-jni.so；三方依赖静态链接进 JNI so，避免打包未对齐的预编译 so。
cmake_minimum_required(VERSION 3.18)
project(asternet-example-android C CXX)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)

set(SDK_CXX     ${ROOT}/sdk/cxx)
set(SDK_THIRD   ${ROOT}/sdk/third_party)
set(SDK_ANDROID ${ROOT}/sdk/android)
set(THIRD_BUILD ${SDK_THIRD}/xquic/third_party/boringssl/build-android-${ANDROID_ABI})

# ===== C++ 源文件 =====
set(ASTERNET_SOURCES
    ${SDK_CXX}/client.cpp
    ${SDK_CXX}/abi/abi.cpp
    ${SDK_CXX}/platform/event_loop.cpp
    ${SDK_CXX}/platform/log.cpp
    ${SDK_CXX}/platform/tls.cpp
    ${SDK_CXX}/connection/connection_pool.cpp
    ${SDK_CXX}/dns/dns_resolver.cpp
    ${SDK_CXX}/sdt/quality_prober.cpp
    ${SDK_CXX}/sdt/traceroute.cpp
    ${SDK_CXX}/orchestrator/interceptor.cpp
    ${SDK_CXX}/monitor/metrics.cpp
    ${SDK_CXX}/protocol/protocol.cpp
    ${SDK_CXX}/engine/http1_engine.cpp
    ${SDK_CXX}/engine/http2_engine.cpp
    ${SDK_CXX}/engine/protocol_selector.cpp
    ${SDK_ANDROID}/jni/asternet_jni.cpp
)

if(ASTERNET_ENABLE_XQUIC)
    list(APPEND ASTERNET_SOURCES ${SDK_CXX}/engine/quic_engine.cpp)
endif()

add_library(asternet-jni SHARED ${ASTERNET_SOURCES})

# ===== include 路径 =====
target_include_directories(asternet-jni PRIVATE
    ${SDK_CXX}/public
    ${SDK_CXX}
    ${SDK_THIRD}/nghttp2/lib/includes
    ${SDK_THIRD}/nghttp2/build-android-${ANDROID_ABI}/lib/includes
    ${SDK_THIRD}/xquic/third_party/boringssl/include
)

if(ASTERNET_ENABLE_XQUIC)
    target_include_directories(asternet-jni PRIVATE ${SDK_THIRD}/xquic/include)
    target_compile_definitions(asternet-jni PRIVATE ASTERNET_ENABLE_XQUIC=1)
endif()

target_compile_definitions(asternet-jni PRIVATE ASTERNET_EXPORTS=1 ASTERNET_PLATFORM_ANDROID=1)
target_compile_features(asternet-jni PUBLIC cxx_std_17)

# 16KB 页面大小支持（Android 15+ / API 35+）
if(ASTERNET_SUPPORT_FLEXIBLE_PAGE_SIZES)
    target_link_options(asternet-jni PRIVATE -Wl,-z,max-page-size=16384)
endif()

# ===== 静态链接三方库 =====
set(NGHTTP2_LIB ${SDK_THIRD}/nghttp2/build-android-${ANDROID_ABI}/lib/libnghttp2.a)
set(BSSL_DIR    ${SDK_THIRD}/xquic/third_party/boringssl/build-android-${ANDROID_ABI})

add_library(nghttp2_imp STATIC IMPORTED)
set_target_properties(nghttp2_imp PROPERTIES IMPORTED_LOCATION ${NGHTTP2_LIB})

add_library(bssl_ssl_imp STATIC IMPORTED)
set_target_properties(bssl_ssl_imp PROPERTIES IMPORTED_LOCATION ${BSSL_DIR}/libssl.a)

add_library(bssl_crypto_imp STATIC IMPORTED)
set_target_properties(bssl_crypto_imp PROPERTIES IMPORTED_LOCATION ${BSSL_DIR}/libcrypto.a)

target_link_libraries(asternet-jni PRIVATE
    nghttp2_imp
    bssl_ssl_imp
    bssl_crypto_imp
    log)

if(ASTERNET_ENABLE_XQUIC)
    set(XQUIC_LIB_DIR ${SDK_THIRD}/xquic/build-android-${ANDROID_ABI})
    if(NOT EXISTS ${XQUIC_LIB_DIR}/libxquic-static.a)
        message(FATAL_ERROR "Missing: ${XQUIC_LIB_DIR}/libxquic-static.a — run sdk/third_party/repack.sh first")
    endif()
    add_library(xquic_imp STATIC IMPORTED)
    set_target_properties(xquic_imp PROPERTIES IMPORTED_LOCATION ${XQUIC_LIB_DIR}/libxquic-static.a)
    target_link_libraries(asternet-jni PRIVATE xquic_imp)
endif()

set_target_properties(asternet-jni PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden)

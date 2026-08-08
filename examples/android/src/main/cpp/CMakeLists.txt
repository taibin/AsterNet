# AsterNet Android Example CMake
# 编译 libasternet-jni.so（当前静态链接三方库，.so 文件通过 jniLibs 单独打包）
cmake_minimum_required(VERSION 3.18)
project(asternet-example-android C CXX)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)

set(SDK_CXX     ${ROOT}/sdk/cxx)
set(SDK_THIRD   ${ROOT}/sdk/third_party)
set(SDK_ANDROID ${ROOT}/sdk/android)

# ===== C++ 源文件 =====
set(ASTERNET_SOURCES
    ${SDK_CXX}/client.cpp
    ${SDK_CXX}/abi/abi.cpp
    ${SDK_CXX}/platform/event_loop.cpp
    ${SDK_CXX}/platform/log.cpp
    ${SDK_CXX}/platform/tls.cpp
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
    ${SDK_THIRD}/nghttp2/build-android-arm64/lib/includes
    ${SDK_THIRD}/xquic/third_party/boringssl/include
)

if(ASTERNET_ENABLE_XQUIC)
    target_include_directories(asternet-jni PRIVATE ${SDK_THIRD}/xquic/include)
    target_compile_definitions(asternet-jni PRIVATE ASTERNET_ENABLE_XQUIC=1)
endif()

target_compile_definitions(asternet-jni PRIVATE ASTERNET_EXPORTS=1)
target_compile_features(asternet-jni PUBLIC cxx_std_17)

# ===== 静态链接三方库（构建可靠；独立 .so 由 jniLibs 打包）=====
set(NGHTTP2_LIB ${SDK_THIRD}/nghttp2/build-android-arm64/lib/libnghttp2.a)
set(BSSL_DIR    ${SDK_THIRD}/xquic/third_party/boringssl/build-android-arm64)

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
    set(XQUIC_LIB_DIR ${SDK_THIRD}/xquic/build-android-arm64)
    if(NOT EXISTS ${XQUIC_LIB_DIR}/libxquic-static.a)
        message(FATAL_ERROR "Missing: ${XQUIC_LIB_DIR}/libxquic-static.a")
    endif()
    add_library(xquic_imp STATIC IMPORTED)
    set_target_properties(xquic_imp PROPERTIES IMPORTED_LOCATION ${XQUIC_LIB_DIR}/libxquic-static.a)
    target_link_libraries(asternet-jni PRIVATE xquic_imp)
endif()

set_target_properties(asternet-jni PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden)

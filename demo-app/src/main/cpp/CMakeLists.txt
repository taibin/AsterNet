# asternet Demo CMake
# 编译 asternet-core + JNI → libasternet-jni.so（包含 H1/H2/H3）
cmake_minimum_required(VERSION 3.18)
project(asternet-demo C CXX)

# 项目根目录（CMakeLists.txt 在 demo-app/src/main/cpp/，根在 ../../../..）
get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. ABSOLUTE)

# ===== C++ 源文件 =====
set(ASTERNET_SOURCES
    ${ROOT}/src/client.cpp
    ${ROOT}/src/abi/abi.cpp
    ${ROOT}/src/platform/event_loop.cpp
    ${ROOT}/src/platform/log.cpp
    ${ROOT}/src/platform/tls.cpp
    ${ROOT}/src/engine/http1_engine.cpp
    ${ROOT}/src/engine/http2_engine.cpp
    ${ROOT}/src/engine/protocol_selector.cpp
    ${ROOT}/android/jni/asternet_jni.cpp
)

if(ASTERNET_ENABLE_XQUIC)
    list(APPEND ASTERNET_SOURCES ${ROOT}/src/engine/quic_engine.cpp)
endif()

add_library(asternet-jni SHARED ${ASTERNET_SOURCES})

# ===== include 路径 =====
target_include_directories(asternet-jni PRIVATE
    ${ROOT}/include
    ${ROOT}/src
    ${ROOT}/third_party/nghttp2/lib/includes
    ${ROOT}/third_party/xquic/third_party/boringssl/include
)

if(ASTERNET_ENABLE_XQUIC)
    target_include_directories(asternet-jni PRIVATE ${ROOT}/third_party/xquic/include)
    target_compile_definitions(asternet-jni PRIVATE ASTERNET_ENABLE_XQUIC=1)
endif()

# C ABI 符号导出：确保 asternet_* 函数在 .dynsym 表中可见
target_compile_definitions(asternet-jni PRIVATE ASTERNET_EXPORTS=1)

target_compile_features(asternet-jni PUBLIC cxx_std_17)

# ===== 链接第三方静态库（需事先交叉编译） =====
# 用 IMPORTED 方式确保 CMake 找到绝对路径
set(NGHTTP2_LIB ${ROOT}/third_party/nghttp2/build-android-arm64/libnghttp2.a)
set(BSSL_DIR    ${ROOT}/third_party/xquic/third_party/boringssl/build-android-arm64)

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
    if(NOT XQUIC_LIB_DIR)
        set(XQUIC_LIB_DIR ${ROOT}/third_party/xquic/build-android-arm64)
    endif()
    if(NOT EXISTS ${XQUIC_LIB_DIR}/libxquic-static.a)
        message(FATAL_ERROR "缺少 Android XQUIC 静态库: ${XQUIC_LIB_DIR}/libxquic-static.a")
    endif()
    add_library(xquic_imp STATIC IMPORTED)
    set_target_properties(xquic_imp PROPERTIES IMPORTED_LOCATION ${XQUIC_LIB_DIR}/libxquic-static.a)
    target_link_libraries(asternet-jni PRIVATE xquic_imp)
endif()

# ===== 符号可见性 =====
set_target_properties(asternet-jni PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden)

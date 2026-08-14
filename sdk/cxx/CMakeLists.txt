# AsterNet C++ 核心库
# 产物：libasternet（动态/静态，端侧 JNI/ObjC++ 壳链接）
#
# 各业务模块（engine/connection/dns/...）随阶段推进逐步加入。

set(ASTERNET_CORE_SOURCES
    client.cpp
    abi/abi.cpp
    platform/event_loop.cpp
    platform/log.cpp
    platform/tls.cpp
    connection/connection_pool.cpp
    dns/dns_resolver.cpp
    sdt/quality_prober.cpp
    orchestrator/interceptor.cpp
    monitor/metrics.cpp
    protocol/protocol.cpp
    engine/http1_engine.cpp
    engine/http2_engine.cpp
    engine/protocol_selector.cpp
)

if(ASTERNET_ENABLE_XQUIC)
    list(APPEND ASTERNET_CORE_SOURCES engine/quic_engine.cpp)
endif()

if(ASTERNET_SHARED)
    set(ASTERNET_LIB_TYPE SHARED)
else()
    set(ASTERNET_LIB_TYPE STATIC)
endif()
add_library(asternet ${ASTERNET_LIB_TYPE} ${ASTERNET_CORE_SOURCES})

# 离线单元测试直接验证内部 C++ 模块。仅静态测试库放宽符号可见性；发布的共享库
# 继续使用根 CMake 配置的隐藏符号策略。
if(ASTERNET_BUILD_TESTS AND NOT ASTERNET_SHARED)
    set_target_properties(asternet PROPERTIES
        C_VISIBILITY_PRESET default
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF)
endif()

# 公共头（对外发布，端侧 include 此目录）
target_include_directories(asternet
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(asternet PUBLIC cxx_std_17)

# ---- 第三方依赖：nghttp2 + boringssl（HTTP/1.1 + HTTP/2 必需）----
get_filename_component(THIRD ${CMAKE_CURRENT_SOURCE_DIR}/../third_party ABSOLUTE)
set(NGHTTP2_ROOT ${THIRD}/nghttp2)
set(BSSL_ROOT ${THIRD}/xquic/third_party/boringssl)
target_include_directories(asternet PRIVATE
    ${NGHTTP2_ROOT}/lib/includes
    ${BSSL_ROOT}/include)

# nghttp2ver.h/config.h 由 nghttp2 的构建过程生成；允许 Android/桌面构建
# 通过 NGHTTP2_GENERATED_INCLUDE_DIR 指向对应架构的生成目录。
if(NGHTTP2_GENERATED_INCLUDE_DIR)
    target_include_directories(asternet PRIVATE ${NGHTTP2_GENERATED_INCLUDE_DIR})
else()
    set(_nghttp2_generated_include_dirs)
    if(ANDROID_ABI)
        list(APPEND _nghttp2_generated_include_dirs
             "${NGHTTP2_ROOT}/build-android-${ANDROID_ABI}/lib/includes"
             "${NGHTTP2_ROOT}/build-android-${ANDROID_ABI}/lib"
             "${NGHTTP2_ROOT}/build-android-${ANDROID_ABI}")
    endif()
    list(APPEND _nghttp2_generated_include_dirs
         "${NGHTTP2_ROOT}/build/lib/includes"
         "${NGHTTP2_ROOT}/build/lib"
         "${NGHTTP2_ROOT}/build")
    foreach(_dir ${_nghttp2_generated_include_dirs})
        if(EXISTS "${_dir}/nghttp2/nghttp2ver.h" OR EXISTS "${_dir}/config.h")
            target_include_directories(asternet PRIVATE "${_dir}")
        endif()
    endforeach()
endif()

if(NOT NGHTTP2_LIB_DIR)
    if(EXISTS "${NGHTTP2_ROOT}/build-android-${ANDROID_ABI}/lib/libnghttp2.a")
        set(NGHTTP2_LIB_DIR ${NGHTTP2_ROOT}/build-android-${ANDROID_ABI}/lib)
    else()
        set(NGHTTP2_LIB_DIR ${NGHTTP2_ROOT}/build)
    endif()
endif()
if(NOT BSSL_LIB_DIR)
    if(EXISTS "${BSSL_ROOT}/build-android-${ANDROID_ABI}/libssl.a")
        set(BSSL_LIB_DIR ${BSSL_ROOT}/build-android-${ANDROID_ABI})
    else()
        set(BSSL_LIB_DIR ${BSSL_ROOT}/build)
    endif()
endif()

macro(asternet_add_imported_lib name path)
    get_filename_component(_abs ${path} ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
    add_library(${name} STATIC IMPORTED)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION ${_abs})
endmacro()

set(ASTERNET_THIRD_PARTY_LIBS "")
asternet_add_imported_lib(nghttp2_imp ${NGHTTP2_LIB_DIR}/libnghttp2.a)
list(APPEND ASTERNET_THIRD_PARTY_LIBS nghttp2_imp)
asternet_add_imported_lib(bssl_ssl_imp ${BSSL_LIB_DIR}/libssl.a)
list(APPEND ASTERNET_THIRD_PARTY_LIBS bssl_ssl_imp)
asternet_add_imported_lib(bssl_crypto_imp ${BSSL_LIB_DIR}/libcrypto.a)
list(APPEND ASTERNET_THIRD_PARTY_LIBS bssl_crypto_imp)
target_link_libraries(asternet PRIVATE ${ASTERNET_THIRD_PARTY_LIBS})

# 平台宏
if(ANDROID)
    target_compile_definitions(asternet PRIVATE ASTERNET_PLATFORM_ANDROID=1)
    target_link_libraries(asternet PRIVATE log)
    if(ASTERNET_SUPPORT_FLEXIBLE_PAGE_SIZES)
        target_link_options(asternet PRIVATE -Wl,-z,max-page-size=16384)
    endif()
elseif(APPLE)
    target_compile_definitions(asternet PRIVATE ASTERNET_PLATFORM_APPLE=1)
elseif(UNIX)
    target_compile_definitions(asternet PRIVATE ASTERNET_PLATFORM_LINUX=1)
    target_link_libraries(asternet PUBLIC pthread)
endif()

# XQUIC
if(ASTERNET_ENABLE_XQUIC)
    set(XQUIC_ROOT ${THIRD}/xquic)
    target_include_directories(asternet PRIVATE
        ${XQUIC_ROOT}/include
        ${XQUIC_ROOT}/third_party/boringssl/include)
    if(NOT XQUIC_LIB_DIR)
        if(EXISTS "${XQUIC_ROOT}/build-android-${ANDROID_ABI}/libxquic-static.a")
            set(XQUIC_LIB_DIR ${XQUIC_ROOT}/build-android-${ANDROID_ABI})
        else()
            set(XQUIC_LIB_DIR ${XQUIC_ROOT}/build)
        endif()
    endif()
    asternet_add_imported_lib(xquic_imp ${XQUIC_LIB_DIR}/libxquic-static.a)
    target_link_libraries(asternet PRIVATE xquic_imp)
    target_compile_definitions(asternet PRIVATE ASTERNET_ENABLE_XQUIC=1)
endif()

# 符号导出
if(ASTERNET_SHARED)
    target_compile_definitions(asternet PRIVATE ASTERNET_EXPORTS=1)
endif()

install(TARGETS asternet
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# AsterNet 网络核心 C++ 库
# 产物：libasternet-core（动态库，端侧 JNI/ObjC++ 壳链接）
#
# 阶段 0：仅编译最小可链接核心（client + abi）。各业务模块（engine/connection/dns/...）
# 的实现随阶段推进逐步加入 ASTERNET_CORE_SOURCES。

# 已实现的源文件（随阶段推进追加）
set(ASTERNET_CORE_SOURCES
    client.cpp
    abi/abi.cpp
    platform/event_loop.cpp
    platform/log.cpp
    platform/tls.cpp
    engine/http1_engine.cpp
    engine/http2_engine.cpp
    engine/protocol_selector.cpp
)

# XQUIC 引擎：ASTERNET_ENABLE_XQUIC 时编译 quic_engine 并链接 xquic + boringssl
if(ASTERNET_ENABLE_XQUIC)
    list(APPEND ASTERNET_CORE_SOURCES engine/quic_engine.cpp)
endif()

# 库类型：POC/桌面测试用静态（省 rpath），端侧打包开 ASTERNET_SHARED=ON 出动态库
if(ASTERNET_SHARED)
    set(ASTERNET_LIB_TYPE SHARED)
else()
    set(ASTERNET_LIB_TYPE STATIC)
endif()
add_library(asternet-core ${ASTERNET_LIB_TYPE} ${ASTERNET_CORE_SOURCES})

target_include_directories(asternet-core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include        # 公共 C ABI 头
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}        # 内部头（client.h 等）
)

target_compile_features(asternet-core PUBLIC cxx_std_17)

# ---- 第三方依赖：nghttp2 + boringssl（HTTP/1.1 + HTTP/2 必需）----
set(NGHTTP2_ROOT ${CMAKE_SOURCE_DIR}/third_party/nghttp2)
set(BSSL_ROOT ${CMAKE_SOURCE_DIR}/third_party/xquic/third_party/boringssl)
target_include_directories(asternet-core PRIVATE
    ${NGHTTP2_ROOT}/lib/includes
    ${BSSL_ROOT}/include)
if(NOT NGHTTP2_LIB_DIR)
    set(NGHTTP2_LIB_DIR ${NGHTTP2_ROOT}/build)
endif()
if(NOT BSSL_LIB_DIR)
    set(BSSL_LIB_DIR ${BSSL_ROOT}/build)
endif()

# 用 IMPORTED 库链接静态 .a，避免 CMake 把路径当 -l 库名（Android toolchain 坑）
# IMPORTED_LOCATION 必须是绝对路径，否则 make 找不到
set(_asternet_thirdparty_libs "")
macro(asternet_add_imported_lib name path)
    get_filename_component(_abs ${path} ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
    add_library(${name} STATIC IMPORTED)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION ${_abs})
    list(APPEND _asternet_thirdparty_libs ${name})
endmacro()
asternet_add_imported_lib(nghttp2_imp ${NGHTTP2_LIB_DIR}/libnghttp2.a)
asternet_add_imported_lib(bssl_ssl_imp ${BSSL_LIB_DIR}/libssl.a)
asternet_add_imported_lib(bssl_crypto_imp ${BSSL_LIB_DIR}/libcrypto.a)
target_link_libraries(asternet-core PRIVATE ${_asternet_thirdparty_libs})

# 平台宏
if(ANDROID)
    target_compile_definitions(asternet-core PRIVATE ASTERNET_PLATFORM_ANDROID=1)
elseif(APPLE)
    target_compile_definitions(asternet-core PRIVATE ASTERNET_PLATFORM_APPLE=1)
elseif(UNIX)
    target_compile_definitions(asternet-core PRIVATE ASTERNET_PLATFORM_LINUX=1)
endif()

# XQUIC 引入（POC 阶段开启，详见 docs/XQUIC_POC_CHECKLIST.md）
if(ASTERNET_ENABLE_XQUIC)
    set(XQUIC_ROOT ${CMAKE_SOURCE_DIR}/third_party/xquic)
    # xquic + boringssl 公共头
    target_include_directories(asternet-core PRIVATE
        ${XQUIC_ROOT}/include
        ${XQUIC_ROOT}/third_party/boringssl/include)

    # 链接预编译的 xquic + boringssl 静态库（由 POC 构建脚本产出，路径可覆盖）
    # 桌面默认 build/；NDK 交叉编译时传 -DXQUIC_LIB_DIR=.../build-android-arm64
    if(NOT XQUIC_LIB_DIR)
        set(XQUIC_LIB_DIR ${XQUIC_ROOT}/build)
    endif()
    if(NOT BSSL_LIB_DIR)
        set(BSSL_LIB_DIR ${XQUIC_ROOT}/third_party/boringssl/build)
    endif()
    asternet_add_imported_lib(xquic_imp ${XQUIC_LIB_DIR}/libxquic-static.a)
    target_link_libraries(asternet-core PRIVATE xquic_imp)

    target_compile_definitions(asternet-core PRIVATE ASTERNET_ENABLE_XQUIC=1)
endif()

# Android bionic 内置 pthread/dl，无需链接；Linux 桌面需 pthread
if(UNIX AND NOT ANDROID)
    target_link_libraries(asternet-core PUBLIC pthread)
elseif(ANDROID)
    target_link_libraries(asternet-core PRIVATE log)
endif()

# 符号导出控制：仅 ABI 符号可见（visibility hidden 已在顶层设置，
# 此处用 ASTERNET_API 宏在 abi.cpp 中显式导出 asternet_* 符号。静态库无需导出宏。
if(ASTERNET_SHARED)
    target_compile_definitions(asternet-core PRIVATE ASTERNET_EXPORTS=1)
endif()

# 安装
install(TARGETS asternet-core
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

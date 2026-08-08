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

# 公共头（对外发布，端侧 include 此目录）
target_include_directories(asternet
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/public
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(asternet PUBLIC cxx_std_17)

# ---- 第三方依赖：nghttp2 + boringssl（HTTP/1.1 + HTTP/2 必需）----
set(THIRD ${CMAKE_SOURCE_DIR}/sdk/third_party)
set(NGHTTP2_ROOT ${THIRD}/nghttp2)
set(BSSL_ROOT ${THIRD}/xquic/third_party/boringssl)
target_include_directories(asternet PRIVATE
    ${NGHTTP2_ROOT}/lib/includes
    ${BSSL_ROOT}/include)

if(NOT NGHTTP2_LIB_DIR)
    set(NGHTTP2_LIB_DIR ${NGHTTP2_ROOT}/build)
endif()
if(NOT BSSL_LIB_DIR)
    set(BSSL_LIB_DIR ${BSSL_ROOT}/build)
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
        set(XQUIC_LIB_DIR ${XQUIC_ROOT}/build)
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

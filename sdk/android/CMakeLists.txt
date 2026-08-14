# AsterNet Android JNI 壳
# 产物：libasternet-jni.so（JNI 桥接，链接 16KB 对齐的 libasternet.so）
cmake_minimum_required(VERSION 3.18)
project(asternet-sdk-android C CXX)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

if(NOT TARGET asternet)
    add_subdirectory(${ROOT}/sdk/cxx ${CMAKE_CURRENT_BINARY_DIR}/asternet-cxx)
endif()

add_library(asternet-jni SHARED
    jni/asternet_jni.cpp)

target_include_directories(asternet-jni PRIVATE
    ${ROOT}/sdk/cxx/public    # asternet.h
    ${ROOT}/sdk/cxx            # client.h 等内部头
    ${ROOT}/sdk/third_party/nghttp2/lib/includes
    ${ROOT}/sdk/third_party/xquic/third_party/boringssl/include)

if(ASTERNET_ENABLE_XQUIC)
    target_include_directories(asternet-jni PRIVATE
        ${ROOT}/sdk/third_party/xquic/include)
    target_compile_definitions(asternet-jni PRIVATE ASTERNET_ENABLE_XQUIC=1)
endif()

target_link_libraries(asternet-jni PRIVATE asternet log)
target_compile_features(asternet-jni PUBLIC cxx_std_17)
target_compile_definitions(asternet-jni PRIVATE ASTERNET_EXPORTS=1)

if(ASTERNET_SUPPORT_FLEXIBLE_PAGE_SIZES)
    target_link_options(asternet-jni PRIVATE -Wl,-z,max-page-size=16384)
endif()

set_target_properties(asternet-jni PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden)

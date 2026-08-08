# AsterNet HarmonyOS NAPI build
cmake_minimum_required(VERSION 3.18)
project(asternet_napi)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.. ABSOLUTE)

add_library(asternet_napi SHARED napi_init.cpp)

target_include_directories(asternet_napi PRIVATE
    ${ROOT}/sdk/cxx/public
    ${ROOT}/sdk/cxx)

# TODO: 链接 libasternet.so + 三方 .so（需先交叉编译）
# target_link_libraries(asternet_napi PRIVATE asternet log)

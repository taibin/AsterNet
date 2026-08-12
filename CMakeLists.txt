# CMake 最低版本：3.18（Android NDK r23+ / Xcode 13+ 通用）
cmake_minimum_required(VERSION 3.18)

# ---- 工程定义 ----
project(asternet
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "AsterNet cross-platform network core (C++)")

# ---- 全局选项 ----
# 默认 C++17（NDK/Xcode 通用支持，规避 C++20 协程跨编译器差异）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 隐藏符号可见性：默认隐藏，仅 ABI 导出符号可见（控体积、防符号冲突）
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(ASTERNET_BUILD_TESTS "Build C++ unit tests" ON)
option(ASTERNET_BUILD_ANDROID "Build Android JNI shim" OFF)
option(ASTERNET_BUILD_IOS     "Build iOS ObjC++ shim" OFF)
option(ASTERNET_ENABLE_XQUIC  "Enable XQUIC engine (POC 阶段引入)" OFF)
option(ASTERNET_ENABLE_LTO    "Enable LTO in Release" ON)
option(ASTERNET_SHARED        "Build shared library (OFF=static, 便于桌面测试)" OFF)
option(ASTERNET_REGISTER_NETWORK_TESTS "Register tests that access the public internet" OFF)

# ---- 构建类型 ----
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Release 开启 LTO + strip，控体积
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND ASTERNET_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
    if(lto_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

include(GNUInstallDirs)

# ---- 辅助模块 ----
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---- 核心库 ----
add_subdirectory(sdk/cxx)

# ---- 测试 ----
if(ASTERNET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(examples/cxx)
endif()

# ---- 端侧壳 ----
if(ASTERNET_BUILD_ANDROID)
    add_subdirectory(sdk/android)
endif()

if(ASTERNET_BUILD_IOS)
    add_subdirectory(sdk/ios)
endif()

# ---- 安装 ----
install(DIRECTORY sdk/cxx/public/asternet DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/asternet)

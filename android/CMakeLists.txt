# Android JNI 壳：构建 libasternet-jni.so
#
# 经 AGP externalNativeBuild 集成（见 README.md）。需 Android NDK。
# 链接静态 asternet-core，产出单一 .so，便于端侧打包与裁剪。

set(ASTERNET_JNI_SOURCES
    jni/asternet_jni.cpp
)

add_library(asternet-jni SHARED ${ASTERNET_JNI_SOURCES})

target_link_libraries(asternet-jni PRIVATE asternet-core log)

target_include_directories(asternet-jni PRIVATE ${CMAKE_SOURCE_DIR}/include)

# 限制导出符号：仅 JNI 入口可见
set_target_properties(asternet-jni PROPERTIES CXX_VISIBILITY_PRESET hidden)

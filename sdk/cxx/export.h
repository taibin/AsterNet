/*
 * AsterNet 网络核心 —— 符号导出宏
 * abi.cpp 编译 libasternet-core 时 ASTERNET_EXPORTS=1，asternet_* 符号被导出；
 * 端侧壳包含 asternet.h 时 ASTERNET_EXPORTS 未定义，符号为导入。
 */
#ifndef ASTERNET_EXPORT_H
#define ASTERNET_EXPORT_H

#if defined(_WIN32)
  #define ASTERNET_EXPORT __declspec(dllexport)
  #define ASTERNET_IMPORT __declspec(dllimport)
#else
  #define ASTERNET_EXPORT __attribute__((visibility("default")))
  #define ASTERNET_IMPORT
#endif

#ifdef ASTERNET_EXPORTS
  #define ASTERNET_API ASTERNET_EXPORT
#else
  #define ASTERNET_API ASTERNET_IMPORT
#endif

#endif /* ASTERNET_EXPORT_H */

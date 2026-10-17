#pragma once

// tiny-lsm 共享库（lsm_shared）符号可见性宏
//
// 用法：在需要对外导出的类 / 函数前加 TINYLSM_API，例如
//     class TINYLSM_API LSM { ... };
//
// Windows 下三种场景：
//   1) 编译 lsm_shared 本身：xmake.lua 定义 TINYLSM_EXPORTS
//      -> __declspec(dllexport)，把符号写进 DLL 导出表
//   2) 外部程序以动态方式链接 lsm_shared：编译时定义 TINYLSM_USE_DLL
//      -> __declspec(dllimport)，从导入库解析符号
//   3) 静态链接（本项目各静态库 target / 测试 / 可执行 / Windows 下的
//      lsm_pybind 都走静态库 lsm.lib）：两个宏都不定义 -> 空
//
// 非 Windows（GCC/Clang）下使用 default 可见性，配合 -fvisibility=hidden。

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(TINYLSM_EXPORTS)
// 编译动态库
#define TINYLSM_API __declspec(dllexport)
#elif defined(TINYLSM_USE_DLL)
// 以动态方式使用动态库
#define TINYLSM_API __declspec(dllimport)
#else
// 静态链接
#define TINYLSM_API
#endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#define TINYLSM_API __attribute__((visibility("default")))
#else
#define TINYLSM_API
#endif

# tiny-lsm Windows 构建问题修复记录

> 修复日期：2026-09-14
> 项目路径：`D:\tiny-lsm-master`
> 结果：`[100%]: build ok`，全部 31 个目标（静态库 / 动态库 / Python 绑定 / 可执行 / 测试）编译链接通过。

---

## 1. 环境信息

| 组件 | 版本 / 路径 |
|------|-------------|
| 操作系统 | Windows x64 |
| 编译器 | MSVC（Visual Studio 2022 Community，14.42.34433 / cl 19.42） |
| 构建工具 | xmake v3.1.1+HEAD.3ba37a0d4（`D:\xmake`） |
| CMake | 4.4.3（`C:\Program Files\CMake`） |
| Ninja | 1.13.2（WinGet 安装，系统 PATH 中可用） |
| Python | 系统 Python 3.12.4（`C:\Python312`）、xmake 自带 Python 3.14.3 |
| 依赖包 | spdlog 1.17.0、toml11 4.4.0、pybind11 3.0.4、asio 1.36.0、gtest 1.17.0 |

---

## 2. 问题总览

最初 `xmake f -c` / `xmake build` 时，依赖包接连安装失败，随后源码阶段又有若干编译错误。共定位并修复 **5 类问题 + 1 个环境卡死问题**：

| # | 现象 / 报错 | 类别 |
|---|-------------|------|
| 0 | 新的 xmake 命令无任何输出、长时间假死 | 残留进程持锁 |
| 1 | toml11 / pybind11：`The detected version of Ninja (xmake v3.1.1...)` | CMake 生成器 |
| 2 | pybind11：`Could NOT find Python (missing: Development.Module)` | Python 开发环境 |
| 3 | `std_file.cpp(26): error C2131 表达式的计算结果不是常数` | 源码（MSVC 兼容性） |
| 4 | `server.cpp(1): fatal error C1083: asio.hpp 找不到` | 依赖被注释 |
| 5 | `test_*.cpp: gtest/gtest.h 找不到` | 依赖被注释 |

---

## 3. 逐项根因与修复

### 问题 0：xmake 命令假死（残留进程持锁）

**现象**：执行 `xmake f -c` 后长时间无任何输出，进程不退出。

**根因**：系统中残留两个此前被强杀 / 异常退出的 `xmake.exe` 进程（PID 11324、24172），仍持有包缓存锁，导致新命令阻塞等待。

**修复**：
```powershell
Stop-Process -Name "xmake" -Force
```
随后清理失败的包缓存目录：
```powershell
# 位于 %LOCALAPPDATA%\.xmake\cache\packages\2609 与 %LOCALAPPDATA%\.xmake\packages
# 删除对应包下的 installdir.failed 及空安装目录
```

> 经验：xmake 卡住且无输出时，先查 `Get-Process xmake` 是否有残留进程。

---

### 问题 1：CMake 包安装失败 —— xmake 误把自身当作 Ninja

**现象**：toml11、pybind11 安装日志中 CMake 报：
```
CMake Error:
  The detected version of Ninja (xmake v3.1.1+HEAD..., A cross...
  -- Configuring incomplete, errors occurred!
```
实际执行的命令行为：
```
cmake ... -G Ninja -DCMAKE_MAKE_PROGRAM=D:\xmake\xmake.exe ...
```

**根因**：xmake 的 `package.cmake_generator.ninja` 策略为这些 CMake 包选择了 Ninja 生成器，但在找不到合适 Ninja 时，把 `CMAKE_MAKE_PROGRAM` 指向了 `xmake.exe` 自身来模拟 Ninja。CMake 检测 Ninja 版本时执行 `<make_program> --version`，读到的却是 xmake 的多行版本横幅，无法解析为版本号，配置直接失败。

**修复**：关闭该策略，让 CMake 依赖包在 Windows/MSVC 下改用 **Visual Studio 生成器（MSBuild）**，从根本上绕开 Ninja 检测。已固化到项目 `xmake.lua` 顶部：
```lua
set_policy("package.cmake_generator.ninja", false)
```

命令行等价写法（一次性）：
```powershell
xmake f -c --policies=package.cmake_generator.ninja:n -y
```

**验证**：策略关闭后，spdlog、toml11 相继 `install ok`。

---

### 问题 2：pybind11 找不到 Python Development.Module

**现象**：
```
Could NOT find Python (missing: Development.Module)
(found suitable version "3.12.4", minimum required is "3.8")
```

**根因（两层叠加）**：

1. **系统 Python 3.12.4 是残缺安装**。`C:\Python312` 下只有 `python.exe` 和运行时 DLL，缺少 `include\`（头文件）、`libs\`（导入库 `python312.lib`）、`Lib\`（标准库）。
   - 因为缺标准库，`python.exe` 无法定位自身安装前缀，把 `sys.prefix` 错误地报告成**当前工作目录**（`D:\tiny-lsm-master`），CMake 据此在错误路径下找头文件/库，必然失败。

2. **解释器与头文件版本混搭**。pybind11 在 Windows 下 `add_deps("python 3.x")`，xmake 自动下载了完整的 Python 3.14.3，并通过 `CMAKE_PREFIX_PATH` 把 **3.14.3 的 include** 传给 CMake；而 pybind11 包安装脚本里的 `find_python3()` 又优先找到了系统 **3.12.4** 作为解释器（`-DPython_EXECUTABLE=C:/Python312/python.exe`）。于是：
   - 解释器 / 导入库 = 3.12.4
   - 头文件目录 = 3.14.3

   版本不一致，CMake 的 `FindPython` 判定 `Development.Module` 不成立。失败构建目录的 `CMakeCache.txt` 可清楚看到这种混搭。

**修复**：

(a) 补全残缺的系统 Python。机器上 `D:\python` 恰好是同一版本（3.12.4）的完整开发文件，用它把缺失部分补齐到 `C:\Python312`：
```powershell
# 补运行时（D:\python 缺 python.exe）
Copy-Item C:\Python312\python.exe, C:\Python312\python312.dll ... D:\python\
# 补开发文件与标准库到 C:\Python312
Copy-Item D:\python\include C:\Python312\include -Recurse -Force
Copy-Item D:\python\libs    C:\Python312\libs    -Recurse -Force
Copy-Item D:\python\Lib     C:\Python312\Lib     -Recurse -Force
Copy-Item D:\python\DLLs\*  C:\Python312\DLLs\   -Recurse -Force
```
补全后验证：
```
C:\Python312\python.exe -c "import sys; print(sys.prefix)"
# 输出 C:\Python312（不再是工作目录）
```
并用最小 CMake 工程确认 `find_package(Python COMPONENTS Development.Module)` 成功。

(b) 修改 pybind11 包定义，让它**优先使用 xmake 依赖的 Python 包**，保证解释器/头文件/库三者版本一致。
文件：`%LOCALAPPDATA%\.xmake\repositories\xmake-repo\packages\p\pybind11\xmake.lua`
将 `on_install` 中获取解释器的逻辑由「直接 `find_python3()`」改为「先取 `package:dep("python")` 的 python.exe，找不到再回退系统」：
```lua
local python
local pydep = package:dep("python")
if pydep then
    local candidates = {
        path.join(pydep:installdir("bin"), is_host("windows") and "python.exe" or "python3"),
        path.join(pydep:installdir(),      is_host("windows") and "python.exe" or "python")
    }
    for _, c in ipairs(candidates) do
        if os.isfile(c) then python = c; break end
    end
end
if not python then python = find_python3() end
```

**验证**：`=> install pybind11 v3.0.4 .. ok`。

---

### 问题 3：std_file.cpp 的 C2131（MSVC 不允许 constexpr + reinterpret_cast）

**现象**：
```
src\utils\std_file.cpp(26): error C2131: 表达式的计算结果不是常数
note: 无法评估的指针值导致了故障
```

**根因**：原代码
```cpp
constexpr void *kInvalidHandle =
    reinterpret_cast<void *>(static_cast<intptr_t>(-1));
```
C++ 标准规定 `reinterpret_cast` 不能出现在常量表达式中；MSVC 严格执行，而 GCC/Clang 作为扩展允许，因此跨平台时在 Windows 暴露。

**修复**（`src/utils/std_file.cpp` 第 26 行）：将 `constexpr` 改为 `void *const`（指针本身为常量、指向非 const，可正常赋给 `void* handle_`）：
```cpp
void *const kInvalidHandle =
    reinterpret_cast<void *>(static_cast<intptr_t>(-1));
```

---

### 问题 4：server 目标缺少 asio

**现象**：`server\src\server.cpp(1): fatal error C1083: 无法打开 "asio.hpp"`。

**根因**：`server.cpp` 第 1–3 行 `#include <asio.hpp>` 等，但 `xmake.lua` 中 asio 依赖被注释掉了。

**修复**（`xmake.lua`，两处）：
```lua
add_requires("asio")                 -- 原为 --add_requires("asio")
-- server 目标内：
add_packages("asio")                 -- 原为 -- add_packages("asio")
```

---

### 问题 5：测试目标缺少 gtest

**现象**：`test\test_block.cpp(5): fatal error C1083: gtest/gtest.h 找不到`（所有 `test_*` 目标均如此）。

**根因**：各测试目标都 `add_packages("gtest", ...)`，但顶层 `add_requires("gtest")` 被注释。

**修复**（`xmake.lua`）：
```lua
add_requires("gtest")                -- 原为 --add_requires("gtest")
```
> `gmock` 按原作者注释「编译失败就注释」保持关闭。

---

## 4. 改动文件清单

| 文件 | 改动 | 是否随仓库持久 |
|------|------|----------------|
| `xmake.lua` | ① 顶部新增 `set_policy("package.cmake_generator.ninja", false)`；② 启用 `add_requires("asio"/"gtest")`；③ server 目标启用 `add_packages("asio")` | 是（项目文件） |
| `src/utils/std_file.cpp` | 第 26 行 `constexpr void*` → `void *const` | 是（源码） |
| `%LOCALAPPDATA%\.xmake\repositories\xmake-repo\packages\p\pybind11\xmake.lua` | on_install 优先使用 xmake 依赖的 Python | **否**，`xrepo update` 可能覆盖，见第 6 节 |
| `C:\Python312`（系统环境） | 从 `D:\python` 补齐 include / libs / Lib / DLLs / python.exe | 系统级 |

### xmake.lua 关键 diff
```diff
 set_project("tiny-lsm")
 set_version("0.0.1")
 set_languages("c++20")
+-- Windows/MSVC 下让 CMake 依赖包使用 Visual Studio 生成器，
+-- 避免 xmake 模拟 ninja 导致版本检测失败
+set_policy("package.cmake_generator.ninja", false)
 ...
---add_requires("asio")
+add_requires("asio")
---add_requires("gtest")
+add_requires("gtest")
 ...
 target("server")
     ...
-  --  add_packages("asio")
+    add_packages("asio")
```

### std_file.cpp diff
```diff
-constexpr void *kInvalidHandle = reinterpret_cast<void *>(static_cast<intptr_t>(-1));
+void *const kInvalidHandle = reinterpret_cast<void *>(static_cast<intptr_t>(-1));
```

---

## 5. 构建结果

最终命令与输出：
```powershell
xmake f -c -y
xmake build -y
# [100%]: build ok
```

产物（位于 `build\windows\x64\release`）：

- **静态库（13）**：logger / config / utils / iterator / skiplist / block / vlog / sst / memtable / wal / lsm / redis / lsm_pybind（`.lib`）
- **动态库 / 绑定**：`lsm_shared.dll`、`lsm_pybind.pyd`
- **可执行程序**：`example.exe`、`debug.exe`、`server.exe`
- **单元测试（13）**：test_config / test_skiplist / test_memtable / test_block / test_blockmeta / test_utils / test_sst / test_lsm / test_block_cache / test_compact / test_redis / test_wal / test_wisckey（`.exe`）

> 编译期存在少量 `C4244 / C4267`（`size_t → int` 隐式转换）**警告**，不影响生成；如需消除，可在 `SearchItem` 构造处统一整型类型。

---

## 6. 后续使用与维护

### 日常命令（策略已固化，无需再带 --policies）
```powershell
xmake                      # 构建
xmake run server           # 运行服务端
xmake run example          # 运行示例
xmake run-all-tests        # 构建并运行全部测试
xmake clean && xmake -r    # 干净重建
```

### 注意事项
1. **pybind11 包补丁可能被覆盖**：若执行 `xrepo update` / 清理 xmake-repo 后 pybind11 再次报 Python 错误，按第 3 节问题 2(b) 重新应用补丁即可。由于系统 Python 3.12.4 已补全，回退风险已降低。
2. **工具链选择**：本机用 MSVC（VS2022）成功。此前尝试 `--toolchain=mingw` 会额外触发 xmake 下载 Python、spdlog 走 MinGW 编译等问题，非必要不要混用工具链；如已混用，执行 `xmake f -c` 回到 MSVC 默认配置。
3. **换机器 / 新环境复现要点**：
   - 保证 Python 为**完整安装**（含 Development：头文件 `Python.h` 与 `pythonXX.lib`）；
   - MSVC 下关闭 `package.cmake_generator.ninja`（本仓库已固化）；
   - asio、gtest 依赖需保持启用。

---

## 7. 附录：关键路径速查

| 用途 | 路径 |
|------|------|
| xmake 全局目录 | `%LOCALAPPDATA%\.xmake` |
| 已安装包 | `%LOCALAPPDATA%\.xmake\packages` |
| 包下载/构建缓存 | `%LOCALAPPDATA%\.xmake\cache\packages\2609` |
| 失败日志 | `<包缓存>\...\installdir.failed\logs\install.txt` |
| xmake-repo 包定义 | `%LOCALAPPDATA%\.xmake\repositories\xmake-repo\packages` |
| 项目构建产物 | `D:\tiny-lsm-master\build\windows\x64\release` |

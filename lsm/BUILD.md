# Tiny-LSM 构建配置文档

> 本文档基于当前实际环境编写：Windows + xmake v3.1.1 + MinGW-w64 GCC 13.2.0

---

## 1. 项目概述

Tiny-LSM 是一个用 C++20 实现的 LSM-Tree 键值存储引擎，支持：

- SkipList / MemTable / SST / WAL 完整 LSM 架构
- WiscKey 大值分离（Value Log）
- MVCC 事务（Read Uncommitted / Read Committed / Repeatable Read）
- Bloom Filter 布隆过滤器
- Block Cache（LRU-K）
- Redis 协议兼容服务端
- Python SDK 绑定

构建系统：**xmake**（非 CMake / Makefile）

---

## 2. 环境要求

| 组件 | 最低要求 | 当前环境 | 状态 |
|------|---------|---------|------|
| 操作系统 | Windows 10/11 或 Linux | Windows 11 | ✅ |
| 构建工具 | xmake ≥ 2.8 | xmake v3.1.1 | ✅ |
| C++ 编译器 | 支持 C++20 | GCC 13.2.0 (MinGW-w64) | ✅ |
| Python（可选） | 3.8+（仅 SDK 需要） | - | 未检测 |

### 2.1 当前编译器详情

```
路径：D:\mingw64\bin\g++.exe
版本：g++.exe (x86_64-posix-seh-rev1, Built by MinGW-Builds project) 13.2.0
线程模型：posix
异常模型：seh
架构：x86_64
```

> ⚠️ MinGW 路径 **不在系统 PATH 中**，需要通过 xmake 显式指定工具链（见第 3 节）。

---

## 3. 工具链配置

### 3.1 方式一：xmake 全局配置（推荐，一次配置永久生效）

在项目根目录执行：

```powershell
# 进入项目目录
cd D:\tiny-lsm-master

# 配置 xmake 使用 mingw 工具链，并指定 MinGW 安装路径
xmake f -p mingw -c --mingw=D:\mingw64
```

参数说明：
- `-p mingw`：指定目标平台为 mingw
- `-c`：清除之前的配置缓存，重新配置
- `--mingw=D:\mingw64`：指定 MinGW 根目录（xmake 会自动找到 `bin\g++.exe`）

配置成功后，xmake 会记住该工具链，后续直接 `xmake` 即可编译。

### 3.2 方式二：将 MinGW 加入 PATH（可选）

如果希望在命令行直接使用 `g++`，可以临时添加 PATH：

```powershell
$env:PATH = "D:\mingw64\bin;" + $env:PATH
```

永久添加（需要管理员权限）：

```powershell
[Environment]::SetEnvironmentVariable("PATH", "D:\mingw64\bin;" + [Environment]::GetEnvironmentVariable("PATH", "User"), "User")
```

### 3.3 验证工具链

```powershell
xmake show -l toolchains | findstr mingw
xmake f -p mingw -c --mingw=D:\mingw64
xmake show
```

`xmake show` 输出中应能看到 `platform: mingw` 和 `arch: x86_64`。

---

## 4. 项目结构

```
D:\tiny-lsm-master\
├── xmake.lua              # 构建脚本（核心配置文件）
├── config.toml            # 运行时配置（LSM参数、Redis前缀、布隆过滤器等）
├── Readme.md              # 项目说明
├── BUILD.md               # 本文档
├── include\               # 公共头文件（对外API）
│   └── lsm\               #   engine.h, level_iterator.h 等
├── src\                   # 源码（按模块分目录，每个模块编译为静态库）
│   ├── logger\            #   日志封装（spdlog）
│   ├── config\            #   TOML配置解析（toml11）
│   ├── utils\             #   工具：布隆过滤器、文件IO、mmap、cursor
│   ├── skiplist\          #   跳表实现
│   ├── iterator\          #   迭代器抽象
│   ├── block\             #   SST数据块、块元信息、块缓存、块迭代器
│   ├── sst\               #   SST文件读写、合并迭代器
│   ├── vlog\              #   WiscKey Value Log
│   ├── memtable\          #   内存表
│   ├── wal\               #   预写日志
│   ├── lsm\               #   LSM引擎核心、事务、层级迭代器
│   └── redis_wrapper\     #   Redis协议封装
├── test\                  # 单元测试（13个测试目标）
├── example\               # 示例程序（main.cpp, debug.cpp）
├── server\                # Redis兼容服务端
├── sdk\                   # Python SDK绑定（pybind11）
├── scripts\               # 辅助脚本（SST可视化等）
└── doc\                   # 文档和图片
```

---

## 5. 构建目标说明

xmake.lua 中定义了以下目标：

### 5.1 静态库模块（内部依赖）

| 目标名 | 源码目录 | 依赖 |
|--------|---------|------|
| `logger` | src/logger | spdlog |
| `config` | src/config | toml11, spdlog |
| `utils` | src/utils | toml11, spdlog |
| `vlog` | src/vlog | utils, config |
| `iterator` | src/iterator | toml11, spdlog |
| `skiplist` | src/skiplist | toml11, spdlog |
| `block` | src/block | config |
| `sst` | src/sst | block, utils, iterator, vlog |
| `memtable` | src/memtable | skiplist, iterator, config, sst |
| `wal` | src/wal | sst, memtable |
| `lsm` | src/lsm | sst, memtable, wal, logger |
| `redis` | src/redis_wrapper | lsm |

### 5.2 可执行文件

| 目标名 | 入口 | 说明 |
|--------|------|------|
| `example` | example/main.cpp | 基础KV操作示例 |
| `debug` | example/debug.cpp | 调试用程序 |
| `server` | server/src/server.cpp | Redis兼容服务端（监听6379） |

### 5.3 测试目标（13个）

`test_config` `test_skiplist` `test_memtable` `test_block` `test_blockmeta`
`test_utils` `test_sst` `test_lsm` `test_block_cache` `test_compact`
`test_redis` `test_wal` `test_wisckey`

### 5.4 共享库与绑定

| 目标名 | 类型 | 说明 |
|--------|------|------|
| `lsm_shared` | 动态库 | Windows 输出 `.dll`，Linux 输出 `.so` |
| `lsm_pybind` | Python模块 | Windows 输出 `.pyd`，需 pybind11 |

---

## 6. 依赖管理

项目通过 xmake 包管理器（xrepo）自动管理依赖，无需手动安装：

| 依赖 | 用途 | 来源 |
|------|------|------|
| `gtest` | 单元测试框架 | xrepo 自动下载 |
| `spdlog` | 日志库（`system=false` 强制使用xrepo版本） | xrepo 自动下载 |
| `toml11` | TOML配置解析 | xrepo 自动下载 |
| `pybind11` | Python绑定（可选，默认注释掉） | 需手动启用 |

> 首次构建时 xmake 会自动下载并编译这些依赖，可能需要几分钟。依赖缓存在 `%USERPROFILE%\.xmake\packages\` 下。

如果网络下载依赖失败，可以配置国内镜像：

```powershell
# 使用 xmake 官方镜像（国内加速）
xmake g --pkg_searchdirs=""
# 或设置代理
$env:HTTP_PROXY = "http://127.0.0.1:7890"
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
```

---

## 7. 构建步骤

### 7.1 完整构建（所有目标）

```powershell
cd D:\tiny-lsm-master

# 第一步：配置工具链（首次或切换配置时执行）
xmake f -p mingw -c --mingw=D:\mingw64

# 第二步：编译
xmake
```

`xmake` 不带参数时默认编译所有目标。输出在 `build\mingw\x86_64\release\` 或 `build\mingw\x86_64\debug\` 目录。

### 7.2 构建指定目标

```powershell
# 只编译 example
xmake build example

# 只编译 lsm 静态库
xmake build lsm

# 只编译所有测试
xmake build --group=tests
```

### 7.3 Debug / Release 模式

```powershell
# Debug 模式（定义 LSM_DEBUG 宏，带调试信息）
xmake f -m debug -c
xmake

# Release 模式（默认，优化编译）
xmake f -m release -c
xmake

# 覆盖率模式（编译加 --coverage 标志）
xmake f -m coverage -c
xmake
```

### 7.4 清理与重建

```powershell
# 清理编译产物
xmake clean

# 清理指定目标
xmake clean example

# 完全清理（包括配置缓存和依赖）
xmake clean -a
# 然后重新配置
xmake f -p mingw -c --mingw=D:\mingw64
```

### 7.5 生成 compile_commands.json（给 clangd 用）

```powershell
xmake project -k compile_commands
```

生成的 `compile_commands.json` 在项目根目录，配合 `.clangd` 文件使用。

---

## 8. 运行测试与示例

### 8.1 运行单个测试

```powershell
xmake run test_lsm
xmake run test_sst
xmake run test_skiplist
```

### 8.2 运行所有测试

```powershell
# 方式一：使用项目自定义任务
xmake run-all-tests

# 方式二：逐个运行
xmake run test_config
xmake run test_skiplist
xmake run test_memtable
xmake run test_block
xmake run test_blockmeta
xmake run test_utils
xmake run test_sst
xmake run test_lsm
xmake run test_block_cache
xmake run test_compact
xmake run test_redis
xmake run test_wal
xmake run test_wisckey
```

### 8.3 运行示例

```powershell
xmake run example
```

示例程序会在当前目录创建 `example_data` 文件夹存储数据。

### 8.4 运行 Redis 兼容服务端

```powershell
xmake run server
```

服务端启动后监听 `127.0.0.1:6379`，可用 `redis-cli` 连接：

```powershell
redis-cli -h 127.0.0.1 -p 6379
> SET key value
> GET key
```

支持的命令：SET/GET/DEL/EXPIRE/TTL、HSET/HGET/HDEL/HKEYS、LPUSH/RPUSH/LPOP/RPOP/LLEN/LRANGE、ZADD/ZREM/ZINCRBY/ZCARD/ZRANGE/ZSCORE/ZRANK、SADD/SREM/SMEMBERS/SISMEMBER/SCARD、FLUSHALL/SAVE。

---

## 9. 安装共享库

```powershell
# 编译并安装 lsm_shared 到系统目录
xmake install --root lsm_shared
```

Windows 下安装结构：
```
<installdir>\
├── include\tiny-lsm\    # 头文件
├── bin\lsm_shared.dll   # 动态库
└── lib\lsm_shared.lib   # 导入库
```

指定安装路径：

```powershell
xmake install --root -o D:\tiny-lsm-install lsm_shared
```

---

## 10. config.toml 运行时配置

项目根目录的 `config.toml` 在运行时被读取，控制引擎行为：

### 10.1 LSM 核心参数 `[lsm.core]`

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `LSM_TOL_MEM_SIZE_LIMIT` | 67108864 (64MB) | 总内存表大小上限 |
| `LSM_PER_MEM_SIZE_LIMIT` | 4194304 (4MB) | 单个内存表大小上限 |
| `LSM_BLOCK_SIZE` | 32768 (32KB) | SST 数据块大小 |
| `LSM_SST_LEVEL_RATIO` | 4 | 层级间大小倍率（Leveled Compaction） |

### 10.2 块缓存 `[lsm.cache]`

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `LSM_BLOCK_CACHE_CAPACITY` | 1024 | 缓存块数量上限 |
| `LSM_BLOCK_CACHE_K` | 8 | LRU-K 算法的 K 值 |

### 10.3 WiscKey 大值分离 `[lsm.wisckey]`

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `WISCKEY_VALUE_THRESHOLD` | 0 | 值大小超过此阈值则写入 VLog；**0 表示禁用** |

启用示例（大于 1KB 的值走 VLog）：
```toml
[lsm.wisckey]
WISCKEY_VALUE_THRESHOLD = 1024
```

### 10.4 布隆过滤器 `[bloom_filter]`

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `BLOOM_FILTER_EXPECTED_SIZE` | 65536 | 预期元素数量 |
| `BLOOM_FILTER_EXPECTED_ERROR_RATE` | 0.1 | 预期误判率 |

### 10.5 Redis 封装 `[redis]`

控制 Redis 数据结构在 LSM 中的 key 前缀和分隔符，一般无需修改。

---

## 11. Python SDK 构建（可选）

> 默认未启用，需要 pybind11 和 Python 开发环境。

### 11.1 启用 pybind11 依赖

编辑 `xmake.lua`，取消第 20 行的注释：

```lua
add_requires("pybind11")
```

### 11.2 构建

```powershell
xmake f -p mingw -c --mingw=D:\mingw64
xmake build lsm_pybind
```

产物为 `lsm_pybind.pyd`（Windows），将其和 `lsm_shared.dll` 放在同一目录即可在 Python 中 `import tinylsm`。

---

## 12. 常见问题排查

### Q1: `error: cannot find -lstdc++` 或链接失败

**原因**：MinGW 路径未正确配置。
**解决**：
```powershell
xmake f -p mingw -c --mingw=D:\mingw64
xmake
```

### Q2: 依赖下载失败 / 超时

**原因**：网络问题，xrepo 下载包超时。
**解决**：
```powershell
# 设置代理后重试
$env:HTTPS_PROXY = "http://127.0.0.1:7890"
xmake f -c
xmake
```

### Q3: `fatal error: spdlog/spdlog.h: No such file or directory`

**原因**：依赖未正确安装或配置缓存损坏。
**解决**：
```powershell
xmake clean -a
xmake f -p mingw -c --mingw=D:\mingw64
xmake
```

### Q4: 编译报错 `std::format` 或 C++20 特性不识别

**原因**：编译器版本过低或语言标准未生效。
**解决**：确认 GCC 版本 ≥ 13（当前 13.2.0 已支持）。xmake.lua 中已设置 `set_languages("c++20")`，无需额外操作。

### Q5: 运行时找不到 `libstdc++-6.dll` / `libgcc_s_seh-1.dll`

**原因**：MinGW 运行时库不在 PATH 中。
**解决**：
```powershell
# 临时添加
$env:PATH = "D:\mingw64\bin;" + $env:PATH
xmake run example
```
或将以下 DLL 复制到可执行文件同目录：
- `D:\mingw64\bin\libstdc++-6.dll`
- `D:\mingw64\bin\libgcc_s_seh-1.dll`
- `D:\mingw64\bin\libwinpthread-1.dll`

### Q6: `xmake run server` 端口被占用

**原因**：6379 端口已被其他程序（如真实 Redis）占用。
**解决**：关闭占用程序，或修改 `server/src/server.cpp` 中的监听端口后重新编译。

### Q7: 测试失败且数据目录残留

测试程序可能在当前目录留下数据文件夹（如 `test_db`、`example_data`）。清理：

```powershell
Remove-Item -Recurse -Force example_data, test_db, *_db -ErrorAction SilentlyContinue
```

---

## 13. 快速开始（一键命令汇总）

```powershell
# 1. 进入项目
cd D:\tiny-lsm-master

# 2. 配置工具链（仅首次需要）
xmake f -p mingw -c --mingw=D:\mingw64

# 3. 编译
xmake

# 4. 跑示例
xmake run example

# 5. 跑全部测试
xmake run-all-tests

# 6. 启动 Redis 服务端
xmake run server
```

---

## 14. 构建产物位置

```
D:\tiny-lsm-master\build\mingw\x86_64\release\
├── liblogger.a          # 各模块静态库
├── libconfig.a
├── libutils.a
├── ...
├── example.exe          # 可执行文件
├── debug.exe
├── server.exe
├── test_lsm.exe         # 测试程序
├── test_sst.exe
└── ...
```

Debug 模式产物在 `build\mingw\x86_64\debug\`。

---

*文档生成时间：2026-09-14 | 基于 xmake.lua 实际配置编写*


# MQ 中间件 Bug 排查日志

## 2026-09-26 | 出题端请求失败（segfault 空指针解引用）

### 现象
- 出题端请求 MQ 中间件（`mq_middleware`，端口 8899）间歇性失败
- dmesg 记录 2 次 segfault：`2026-09-23 04:45:52` 和 `04:45:56`，进程名 `consumer`
- 崩溃地址固定：`ip 0x40c5d1`，error 4 = **读取空指针（nullptr dereference）**
- systemd 在 9月24日自动重启服务 8 次以上，9月26日 12:29 再次触发重启

### 根因分析
- C++ middleware 程序存在空指针解引用 Bug
- 崩溃发生在固定地址，说明是确定性代码路径触发
- 最可能场景：出题端调用 `/api/submit`、`/api/problem/upsert` 等接口时，JSON 字段缺失或数据库查询返回空结果，代码未做空值检查直接访问指针/引用
- 单进程 HTTP 服务，一次 segfault 导致整个进程崩溃，systemd 拉起后相同请求再次触发 → 崩溃重启循环

### 影响
- 崩溃期间出题端所有请求收到 Connection refused / Timeout
- 频繁重启导致服务不稳定，提交数据可能丢失

### 已执行修复措施
1. ✅ **添加 AddressSanitizer**：修改 `CMakeLists.txt`，新增 `ENABLE_ASAN` 选项（默认 ON）
   - 编译标志：`-g -fsanitize=address -fno-omit-frame-pointer`
   - 原文件已备份为 `CMakeLists.txt.bak`
2. ✅ **重新编译并部署**：二进制已包含 `__asan_*` 符号，服务重启成功，端口 8899 正常响应
3. ✅ **验证通过**：当前无 ASan 告警，服务运行正常

### 后续操作
- 当出题端再次触发崩溃时，执行 `sudo journalctl -u mq_middleware --since "最近时间" --no-pager` 查看 ASan 报告
- ASan 输出格式：`==PID==ERROR: AddressSanitizer: SEGV ... #0 0x40c5d1 in func_name file.cpp:line`
- 根据 ASan 报告定位具体代码行，修复空值检查
- 修复后关闭 ASan：`cmake .. -DENABLE_ASAN=OFF && make && sudo systemctl restart mq_middleware`
- ⚠️ ASan 使内存占用增加 2-3 倍、性能降低约 2 倍，仅排查阶段开启

### 关键文件
- 源码：`/home/admin/.openclaw/workspace/mq_middleware/main.cpp`、`business.cpp`
- 配置：`/home/admin/.openclaw/workspace/mq_middleware/config.json`
- 二进制：`/home/admin/.openclaw/workspace/mq_middleware/build/middleware`
- CMake 备份：`/home/admin/.openclaw/workspace/mq_middleware/CMakeLists.txt.bak`

#pragma once
#include <string>
#include <cstdint>
#include <vector>

struct SandboxConfig {
    uint32_t cpu_limit_ms;
    uint64_t mem_limit_bytes;
    int64_t  max_pids;          // 进程炸弹防御：cgroup pids.max
    std::string problem_data;   // 题目数据目录（chdir 目标）
    std::string user_binary;    // 用户可执行文件绝对路径
    std::vector<std::string> args; // 传递给用户程序的 argv
    std::string stdin_file;     // 可选：重定向 stdin 的文件路径
    std::string stdout_file;    // 可选：捕获 stdout 的文件路径
    std::string stderr_file;    // 可选：捕获 stderr 的文件路径
};

// 子进程 exec 后执行的沙箱主函数（不返回，直接 exec 或 _exit）
[[noreturn]] void sandbox_main(const SandboxConfig& config);

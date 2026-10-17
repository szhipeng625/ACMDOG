#include "sandbox.h"
#include "cgroup_manager.h"
#include <seccomp.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// 安装 seccomp BPF 白名单过滤器
static int install_seccomp() {
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL_PROCESS);
    if (!ctx) return -1;

    // 运行时必需的系统调用白名单（覆盖 glibc / libstdc++ / 常见语言运行时）
    int allowed[] = {
        // I/O
        SCMP_SYS(read), SCMP_SYS(write), SCMP_SYS(lseek),
        SCMP_SYS(openat), SCMP_SYS(close), SCMP_SYS(fcntl),
        SCMP_SYS(fstat), SCMP_SYS(newfstatat), SCMP_SYS(stat), SCMP_SYS(lstat),
        SCMP_SYS(access), SCMP_SYS(faccessat),
        SCMP_SYS(getcwd), SCMP_SYS(readlink), SCMP_SYS(readlinkat),
        SCMP_SYS(ioctl), SCMP_SYS(dup), SCMP_SYS(dup2), SCMP_SYS(dup3),
        SCMP_SYS(pipe), SCMP_SYS(pipe2),
        SCMP_SYS(select), SCMP_SYS(pselect6), SCMP_SYS(poll), SCMP_SYS(ppoll),
        SCMP_SYS(epoll_create), SCMP_SYS(epoll_create1), SCMP_SYS(epoll_ctl),
        SCMP_SYS(epoll_wait), SCMP_SYS(epoll_pwait),

        // Memory
        SCMP_SYS(mmap), SCMP_SYS(munmap), SCMP_SYS(mprotect), SCMP_SYS(mremap),
        SCMP_SYS(brk), SCMP_SYS(madvise), SCMP_SYS(mincore),

        // Process / Thread
        SCMP_SYS(clone), SCMP_SYS(clone3), SCMP_SYS(execve),
        SCMP_SYS(wait4), SCMP_SYS(waitid),
        SCMP_SYS(exit), SCMP_SYS(exit_group),
        SCMP_SYS(kill), SCMP_SYS(tkill), SCMP_SYS(tgkill),
        SCMP_SYS(getpid), SCMP_SYS(getppid), SCMP_SYS(gettid),
        SCMP_SYS(getuid), SCMP_SYS(getgid), SCMP_SYS(geteuid), SCMP_SYS(getegid),
        SCMP_SYS(getresuid), SCMP_SYS(getresgid),
        SCMP_SYS(set_tid_address), SCMP_SYS(set_robust_list),
        SCMP_SYS(rseq), SCMP_SYS(prlimit64),
        SCMP_SYS(futex), SCMP_SYS(nanosleep), SCMP_SYS(clock_nanosleep),
        SCMP_SYS(clock_gettime), SCMP_SYS(clock_getres),
        SCMP_SYS(gettimeofday),

        // Signal
        SCMP_SYS(rt_sigaction), SCMP_SYS(rt_sigprocmask),
        SCMP_SYS(rt_sigreturn), SCMP_SYS(rt_sigtimedwait),
        SCMP_SYS(sigaltstack),

        // Misc required by glibc / dynamic linker
        SCMP_SYS(arch_prctl),
        SCMP_SYS(getrandom),
        SCMP_SYS(uname),
        SCMP_SYS(sysinfo),
        SCMP_SYS(getdents64),
        SCMP_SYS(writev), SCMP_SYS(readv),
        SCMP_SYS(pread64), SCMP_SYS(pwrite64),
        SCMP_SYS(fallocate),
        SCMP_SYS(ftruncate),
        SCMP_SYS(fsync), SCMP_SYS(fdatasync),
        SCMP_SYS(getrlimit),
    };

    for (int sys : allowed) {
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, sys, 0) != 0) {
            fprintf(stderr, "[sandbox] seccomp: failed to allow syscall %d\n", sys);
            seccomp_release(ctx);
            return -1;
        }
    }

    int rc = seccomp_load(ctx);
    seccomp_release(ctx);
    return rc;
}

[[noreturn]] void sandbox_main(const SandboxConfig& config) {
    // 1. 切换到题目数据目录
    if (!config.problem_data.empty()) {
        if (chdir(config.problem_data.c_str()) != 0) {
            perror("[sandbox] chdir to problem_data");
            _exit(126);
        }
    }

    // 2. 重定向 stdin/stdout/stderr（在降权和 seccomp 之前完成）
    if (!config.stdin_file.empty()) {
        int fd = open(config.stdin_file.c_str(), O_RDONLY);
        if (fd < 0) { perror("[sandbox] open stdin_file"); _exit(126); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (!config.stdout_file.empty()) {
        int fd = open(config.stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("[sandbox] open stdout_file"); _exit(126); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    if (!config.stderr_file.empty()) {
        int fd = open(config.stderr_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { perror("[sandbox] open stderr_file"); _exit(126); }
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    // 3. 降权到 nobody (uid=65534, gid=65534)
    if (setgid(65534) != 0 || setuid(65534) != 0) {
        perror("[sandbox] drop privileges");
        _exit(126);
    }

    // 4. 设置 wall-clock 超时（通过 RLIMIT_CPU 作为辅助保护）
    if (config.cpu_limit_ms > 0) {
        struct rlimit rl;
        rl.rlim_cur = rl.rlim_max = (config.cpu_limit_ms + 999) / 1000 + 1;
        setrlimit(RLIMIT_CPU, &rl);
    }

    // 5. 安装 seccomp 过滤器（不可逆，必须在 exec 前最后一步）
    if (install_seccomp() != 0) {
        fprintf(stderr, "[sandbox] seccomp load failed\n");
        _exit(126);
    }

    // 6. 构建 argv 并 exec 用户程序
    std::vector<const char*> argv;
    argv.push_back(config.user_binary.c_str());
    for (const auto& a : config.args) {
        argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);

    execv(config.user_binary.c_str(), const_cast<char* const*>(argv.data()));

    // exec 失败才会到这里
    fprintf(stderr, "[sandbox] execv(%s) failed: %s\n",
            config.user_binary.c_str(), strerror(errno));
    _exit(127);
}

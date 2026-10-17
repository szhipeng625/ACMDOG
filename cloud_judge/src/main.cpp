#include "sandbox.h"
#include "cgroup_manager.h"
#include "json.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <atomic>
#include <csignal>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

static std::atomic<bool> g_running{true};
void signal_handler(int) { g_running.store(false); }

struct EvalTask {
    std::string task_id;
    std::string problem_data;
    std::string user_binary;
    std::vector<std::string> args;
    std::string stdin_file;
    uint32_t cpu_limit_ms;
    uint64_t mem_limit_bytes;
    int64_t max_pids;
    std::string expected_output_file;
    std::string checker;   // diff（默认，输出比对）/ exitcode（SPJ：退出码 0=AC）
};

EvalTask parse_task(const std::string& body) {
    auto j = json::parse(body);
    EvalTask t;
    t.task_id         = j.at("task_id").get<std::string>();
    t.user_binary     = j.at("user_binary").get<std::string>();
    t.problem_data    = j.value("problem_data", "");
    t.cpu_limit_ms    = j.value("cpu_limit_ms", 1000u);
    t.mem_limit_bytes = j.value("mem_limit_bytes", 256ULL * 1024 * 1024);
    t.max_pids        = j.value("max_pids", 64LL);
    t.stdin_file      = j.value("stdin_file", "");
    t.expected_output_file = j.value("expected_output_file", "");
    t.checker         = j.value("checker", "diff");
    if (j.contains("args")) {
        for (auto& a : j["args"]) {
            t.args.push_back(a.get<std::string>());
        }
    }
    return t;
}

// 与客户端 judge.cpp 的 normalize 保持一致：去 UTF-8 BOM、去 \r、去行尾空白、去末尾空行。
static std::string normalize_text(const std::string& raw) {
    std::string s = raw;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s = s.substr(3);
    std::string noCR;
    for (char c : s) if (c != '\r') noCR += c;
    std::istringstream iss(noCR);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line)) {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        lines.push_back(line);
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string out;
    for (auto& l : lines) out += l + "\n";
    return out;
}

static bool read_text_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static bool compare_output(const std::string& actual_file, const std::string& expected_file) {
    std::string a, e;
    if (!read_text_file(actual_file, a) || !read_text_file(expected_file, e)) return false;
    return normalize_text(a) == normalize_text(e);
}

json run_eval(const EvalTask& task) {
    std::cout << "[eval] task=" << task.task_id
              << " binary=" << task.user_binary
              << " cpu=" << task.cpu_limit_ms << "ms"
              << " mem=" << task.mem_limit_bytes
              << " pids=" << task.max_pids << std::endl;

    std::string tmp_dir = "/tmp/eval_" + task.task_id;
    fs::create_directories(tmp_dir);
    // permissions set by create_directories (owner rwx)
    std::string stdout_path = tmp_dir + "/stdout.txt";
    std::string stderr_path = tmp_dir + "/stderr.txt";

    std::string cg_path;
    try {
        cg_path = CgroupManager::create_judge(task.task_id, task.cpu_limit_ms,
                                        task.mem_limit_bytes, task.max_pids);
    } catch (const std::exception& e) {
        // fs::remove_all(tmp_dir); // DEBUG
        return {{"task_id", task.task_id}, {"status", "SE"}, {"error", e.what()}};
    }

    pid_t pid = fork();
    if (pid < 0) {
        CgroupManager::destroy(cg_path);
        // fs::remove_all(tmp_dir); // DEBUG
        return {{"task_id", task.task_id}, {"status", "SE"}, {"error", "fork failed"}};
    }

    if (pid == 0) {
        CgroupManager::attach_pid(cg_path, getpid());
        SandboxConfig cfg;
        cfg.cpu_limit_ms    = task.cpu_limit_ms;
        cfg.mem_limit_bytes = task.mem_limit_bytes;
        cfg.max_pids        = task.max_pids;
        cfg.problem_data    = task.problem_data;
        cfg.user_binary     = task.user_binary;
        cfg.args            = task.args;
        cfg.stdin_file      = task.stdin_file;
        cfg.stdout_file     = stdout_path;
        cfg.stderr_file     = stderr_path;
        sandbox_main(cfg); // [[noreturn]]
    }

    // Wall-clock timeout: 3x CPU limit + 2s grace
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(task.cpu_limit_ms * 3 + 2000);
    int wstatus = 0;
    bool wall_timeout = false;

    while (true) {
        pid_t ret = waitpid(pid, &wstatus, WNOHANG);
        if (ret == pid) break;
        if (ret < 0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &wstatus, 0);
            wall_timeout = true;
            break;
        }
        usleep(1000);
    }

    // Collect resource usage via getrusage (reliable, no cgroup delegation needed)
    struct rusage ru;
    getrusage(RUSAGE_CHILDREN, &ru);
    uint64_t cpu_ns = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ULL
                    + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ULL;
    uint64_t mem_bytes = (uint64_t)ru.ru_maxrss * 1024ULL;  // maxrss is in KB on Linux

    // Also try cgroup stats as supplementary (may be 0 if delegation unavailable)
    auto cg_usage = CgroupManager::read_usage(cg_path);
    CgroupManager::destroy(cg_path);

    // Prefer cgroup values if available, fall back to getrusage
    if (cg_usage.cpu_ns > 0) cpu_ns = cg_usage.cpu_ns;
    if (cg_usage.mem_bytes > 0) mem_bytes = cg_usage.mem_bytes;

    std::string stderr_content;
    {
        std::ifstream f(stderr_path);
        if (f.is_open()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            stderr_content = ss.str();
        }
    }

    json result;
    result["task_id"]   = task.task_id;
    result["cpu_ns"]    = cpu_ns;
    result["mem_bytes"] = mem_bytes;
    result["exit_code"] = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;

    bool cpu_exceeded = (cpu_ns > static_cast<uint64_t>(task.cpu_limit_ms) * 1000000000ULL);
    bool mem_exceeded = (mem_bytes > task.mem_limit_bytes);

    if (wall_timeout || cpu_exceeded) {
        result["status"] = "TLE";
    } else if (mem_exceeded) {
        result["status"] = "MLE";
    } else if (WIFSIGNALED(wstatus)) {
        result["status"] = "RE";
        result["signal"] = WTERMSIG(wstatus);
    } else if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        if (task.checker == "exitcode") {
            // SPJ 判定器：非 0 退出 = 不通过（WA），stderr 作为错误信息
            result["status"] = "WA";
            if (!stderr_content.empty())
                result["error"] = stderr_content.substr(0, 2048);
        } else {
            result["status"] = "RE";
            if (!stderr_content.empty())
                result["stderr"] = stderr_content.substr(0, 2048);
        }
    } else {
        if (task.checker == "exitcode") {
            // SPJ 判定器：退出码 0 = 通过
            result["status"] = "AC";
        } else if (!task.expected_output_file.empty()) {
            bool ac = compare_output(stdout_path, task.expected_output_file);
            result["status"] = ac ? "AC" : "WA";
        } else {
            result["status"] = "AC";
        }
    }

    // fs::remove_all(tmp_dir); // DEBUG
    return result;
}

int main() {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "[eval-worker] ready. reading JSON tasks from stdin (one per line)..." << std::endl;
    std::cout << "[eval-worker] send Ctrl+C or EOF to stop." << std::endl;

    std::string line;
    while (g_running.load() && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            auto task = parse_task(line);
            auto result = run_eval(task);
            std::cout << result.dump() << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[eval] error: " << e.what() << std::endl;
        }
    }

    std::cout << "[eval-worker] shutting down." << std::endl;
    return 0;
}

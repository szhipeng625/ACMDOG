#include "json.hpp"
#include "thread_pool.h"
#include "cgroup_manager.h"
#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <sstream>
#include <filesystem>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

enum class TaskStatus { PENDING, COMPILING, RUNNING, FINISHED, ERROR };

std::string status_str(TaskStatus s) {
    switch (s) {
        case TaskStatus::PENDING:   return "pending";
        case TaskStatus::COMPILING: return "compiling";
        case TaskStatus::RUNNING:   return "running";
        case TaskStatus::FINISHED:  return "finished";
        case TaskStatus::ERROR:     return "error";
    }
    return "unknown";
}

struct TaskRecord {
    std::string task_id;
    TaskStatus status;
    std::string result_json;
    std::chrono::steady_clock::time_point created_at;
};

static std::unordered_map<std::string, TaskRecord> g_tasks;
static std::mutex g_mutex;
static std::atomic<bool> g_running{true};
static const char* EVAL_WORKER_BIN = "/home/admin/eval-worker/build/eval_worker";

// ===== Resource Preemption Model =====
static std::atomic<int> g_active_judges{0};
static std::string g_data_prep_cg;

static constexpr int JUDGE_HIGH_WATERMARK = 2;
static constexpr int JUDGE_LOW_WATERMARK  = 1;
static constexpr uint32_t WEIGHT_DATA_PREP_NORMAL   = 50;
static constexpr uint32_t WEIGHT_DATA_PREP_CRUSHED  = 1;
static constexpr uint32_t WEIGHT_JUDGE              = 200;

static std::atomic<uint32_t> g_current_dp_weight{WEIGHT_DATA_PREP_NORMAL};

static void cg_write_safe(const std::string& path, const std::string& value) {
    std::ofstream f(path);
    if (f.is_open()) { f << value; f.flush(); }
}

static void adjust_preemption() {
    int active = g_active_judges.load();
    uint32_t desired;
    if (active >= JUDGE_HIGH_WATERMARK) {
        desired = WEIGHT_DATA_PREP_CRUSHED;
    } else if (active <= JUDGE_LOW_WATERMARK) {
        desired = WEIGHT_DATA_PREP_NORMAL;
    } else {
        return;
    }
    uint32_t current = g_current_dp_weight.load();
    if (desired != current && !g_data_prep_cg.empty()) {
        cg_write_safe(g_data_prep_cg + "/cpu.weight", std::to_string(desired));
        g_current_dp_weight.store(desired);
        std::cout << "[preempt] data_prep weight " << current << " -> " << desired
                  << " (active_judges=" << active << ")" << std::endl;
    }
}

struct JudgeGuard {
    JudgeGuard() {
        g_active_judges.fetch_add(1);
        adjust_preemption();
    }
    ~JudgeGuard() {
        g_active_judges.fetch_sub(1);
        adjust_preemption();
    }
};

void signal_handler(int) { g_running.store(false); }

void update_status(const std::string& id, TaskStatus st, const std::string& result = "") {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_tasks.find(id);
    if (it != g_tasks.end()) {
        it->second.status = st;
        if (!result.empty()) it->second.result_json = result;
    }
    std::cout << "[orch] " << id << " -> " << status_str(st) << std::endl;
}

void execute_task(const std::string& task_id, const std::string& input_json) {
    JudgeGuard guard;

    update_status(task_id, TaskStatus::COMPILING);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    update_status(task_id, TaskStatus::RUNNING);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        update_status(task_id, TaskStatus::ERROR, "{\"error\":\"pipe failed\"}");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        update_status(task_id, TaskStatus::ERROR, "{\"error\":\"fork failed\"}");
        return;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(EVAL_WORKER_BIN, "eval_worker", nullptr);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);

    std::string payload = input_json + "\n";
    write(in_pipe[1], payload.c_str(), payload.size());
    close(in_pipe[1]);

    std::string result_buf;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
    while (true) {
        fd_set fds; FD_ZERO(&fds); FD_SET(out_pipe[0], &fds);
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            kill(pid, SIGKILL);
            int ws; waitpid(pid, &ws, 0);
            close(out_pipe[0]);
            update_status(task_id, TaskStatus::ERROR, "{\"error\":\"timeout after 300s\"}");
            return;
        }
        struct timeval tv;
        tv.tv_sec = remaining.count() / 1000000;
        tv.tv_usec = remaining.count() % 1000000;
        int sel = select(out_pipe[0] + 1, &fds, nullptr, nullptr, &tv);
        if (sel > 0) {
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n <= 0) break;
            result_buf.append(buf, n);
        } else if (sel == 0) {
            continue;
        } else {
            break;
        }
    }
    close(out_pipe[0]);

    int wstatus;
    waitpid(pid, &wstatus, 0);

    if (WIFSIGNALED(wstatus)) {
        int sig = WTERMSIG(wstatus);
        const char* sig_name = strsignal(sig);
        std::string err = "{\"error\":\"killed by signal " + std::to_string(sig);
        if (sig_name) err += std::string(" (") + sig_name + ")";
        err += "\"}";
        update_status(task_id, TaskStatus::ERROR, err);
    } else if (!WIFEXITED(wstatus)) {
        update_status(task_id, TaskStatus::ERROR, "{\"error\":\"abnormal termination\"}");
    } else if (WEXITSTATUS(wstatus) == 127) {
        update_status(task_id, TaskStatus::ERROR,
            "{\"error\":\"eval_worker binary not found or exec failed\"}");
    } else if (WEXITSTATUS(wstatus) != 0) {
        update_status(task_id, TaskStatus::ERROR,
            "{\"error\":\"eval_worker exited with code " +
            std::to_string(WEXITSTATUS(wstatus)) + "\"}");
    } else if (result_buf.empty()) {
        update_status(task_id, TaskStatus::ERROR,
            "{\"error\":\"eval_worker produced no output\"}");
    } else {
        std::string json_line;
        std::istringstream stream(result_buf);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line[0] == '{') {
                json_line = line;
            }
        }
        if (json_line.empty()) {
            update_status(task_id, TaskStatus::ERROR,
                "{\"error\":\"eval_worker produced no JSON output\"}");
        } else {
            update_status(task_id, TaskStatus::FINISHED, json_line);
        }
    }
}

void http_status_server(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { std::cerr << "[http] socket failed\n"; return; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[http] bind :" << port << " failed\n"; close(srv); return;
    }
    listen(srv, 16);
    std::cout << "[http] status server on 127.0.0.1:" << port << std::endl;
    while (g_running.load()) {
        sockaddr_in cli{}; socklen_t clen = sizeof(cli);
        int fd = accept(srv, (sockaddr*)&cli, &clen);
        if (fd < 0) continue;
        char req[2048]; int rn = read(fd, req, sizeof(req)-1);
        if (rn <= 0) { close(fd); continue; }
        req[rn] = '\0';
        std::string body;
        std::string path;
        { std::istringstream iss(req); std::string method; iss >> method >> path; }
        if (path == "/status") {
            std::lock_guard<std::mutex> lk(g_mutex);
            json arr = json::array();
            for (auto& [id, rec] : g_tasks) {
                arr.push_back({
                    {"task_id", id},
                    {"status", status_str(rec.status)},
                    {"result", rec.result_json.empty() ? nullptr : json::parse(rec.result_json, nullptr, false)}
                });
            }
            body = arr.dump();
        } else if (path == "/preempt") {
            body = json{
                {"active_judges", g_active_judges.load()},
                {"data_prep_weight", g_current_dp_weight.load()},
                {"high_watermark", JUDGE_HIGH_WATERMARK},
                {"low_watermark", JUDGE_LOW_WATERMARK}
            }.dump();
        } else if (path.rfind("/task/", 0) == 0) {
            std::string tid = path.substr(6);
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_tasks.find(tid);
            if (it != g_tasks.end()) {
                body = json{{"task_id", tid},
                            {"status", status_str(it->second.status)},
                            {"result", it->second.result_json.empty() ? nullptr : json::parse(it->second.result_json, nullptr, false)}}.dump();
            } else {
                body = "{\"error\":\"not found\"}";
            }
        } else {
            body = "{\"endpoints\":[\"/status\",\"/task/{id}\",\"/preempt\"]}";
        }
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n"
                           "Connection: close\r\n\r\n" + body;
        write(fd, resp.c_str(), resp.size());
        close(fd);
    }
    close(srv);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        g_data_prep_cg = CgroupManager::init_data_prep();
        CgroupManager::enter_data_prep();
        std::cout << "[orchestrator] entered data_prep cgroup: " << g_data_prep_cg << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[orchestrator] WARNING: cgroup init failed: " << e.what()
                  << " (preemption disabled)" << std::endl;
    }

    int http_port = (argc > 1) ? std::atoi(argv[1]) : 8890;
    size_t pool_size = std::thread::hardware_concurrency();
    if (pool_size == 0) pool_size = 4;
    size_t max_queue = pool_size * 4;

    std::cout << "[orchestrator] starting... http=127.0.0.1:" << http_port
              << " pool=" << pool_size << " queue=" << max_queue
              << " preempt_high=" << JUDGE_HIGH_WATERMARK
              << " preempt_low=" << JUDGE_LOW_WATERMARK << std::endl;
    std::cout << "[orchestrator] reading tasks from stdin (JSON per line)" << std::endl;

    std::thread(http_status_server, http_port).detach();

    ThreadPool pool(pool_size, max_queue);

    std::string line;
    while (g_running.load() && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            std::string tid = j.at("task_id").get<std::string>();
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_tasks[tid] = {tid, TaskStatus::PENDING, "", std::chrono::steady_clock::now()};
            }
            update_status(tid, TaskStatus::PENDING);
            pool.submit([tid, line]() {
                execute_task(tid, line);
            });
        } catch (const std::exception& e) {
            std::cerr << "[orch] parse error: " << e.what() << std::endl;
        }
    }

    std::cout << "[orchestrator] shutting down thread pool..." << std::endl;
    pool.shutdown();
    std::cout << "[orchestrator] stopped." << std::endl;
    return 0;
}

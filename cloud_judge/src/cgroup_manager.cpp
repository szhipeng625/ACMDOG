#include "cgroup_manager.h"
#include <unistd.h>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <iostream>

namespace fs = std::filesystem;

static void cg_write(const std::string& path, const std::string& value) {
    std::ofstream f(path);
    if (!f.is_open())
        throw std::runtime_error("cgroup: cannot open " + path);
    f << value;
    f.flush();
    if (f.fail())
        throw std::runtime_error("cgroup: write failed to " + path + " (value=" + value + ")");
}

static std::string read_cg_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (f.is_open() && std::getline(f, line)) return line;
    return "";
}

// Detect delegate subtree root: prefer cloud_judge.service, fallback to own cgroup
static std::string detect_cg_root() {
    const std::string delegate_path = "/sys/fs/cgroup/system.slice/cloud_judge.service";
    const std::string delegate_path_alt = "/sys/fs/cgroup/system.slice/cloud_judge_api.service";
    if (fs::exists(delegate_path + "/cgroup.procs")) {
        return delegate_path;
    }
    if (fs::exists(delegate_path_alt + "/cgroup.procs")) {
        return delegate_path_alt;
    }
    // Fallback: derive from /proc/self/cgroup
    std::ifstream cg("/proc/self/cgroup");
    std::string line;
    if (cg.is_open()) {
        while (std::getline(cg, line)) {
            auto pos = line.find("::");
            if (pos != std::string::npos) {
                std::string rel = line.substr(pos + 2);
                if (!rel.empty()) {
                    std::string candidate = "/sys/fs/cgroup" + rel;
                    if (fs::exists(candidate)) return candidate;
                }
            }
        }
    }
    throw std::runtime_error("cgroup: no writable cgroup v2 subtree found");
}

static const std::string CG_ROOT = detect_cg_root();

std::string CgroupManager::get_root() { return CG_ROOT; }

// Enable subtree_control on parent (best-effort, may fail if processes exist)
static void enable_subtree_control(const std::string& parent) {
    std::string sc = parent + "/cgroup.subtree_control";
    if (fs::exists(sc)) {
        try { cg_write(sc, "+cpu +memory +pids"); }
        catch (...) { /* non-fatal: already enabled or busy */ }
    }
}

std::string CgroupManager::create_with_weight(const std::string& name,
                                               uint32_t cpu_weight,
                                               uint32_t cpu_limit_ms,
                                               uint64_t mem_limit_bytes,
                                               int64_t max_pids) {
    std::string path = CG_ROOT + "/" + name;
    enable_subtree_control(CG_ROOT);

    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) throw std::runtime_error("cgroup: mkdir " + path + ": " + ec.message());

    // cpu.weight (relative priority, 1-10000, default 100)
    if (cpu_weight > 0) {
        std::string wpath = path + "/cpu.weight";
        if (fs::exists(wpath)) {
            cg_write(wpath, std::to_string(cpu_weight));
        } else {
            std::cerr << "[cgroup] WARNING: " << wpath << " not found (subtree_control not enabled?)" << std::endl;
        }
    }

    // cpu.max (hard limit, only for judge tasks)
    if (cpu_limit_ms > 0) {
        uint64_t quota_us = static_cast<uint64_t>(cpu_limit_ms) * 1000;
        std::string mpath = path + "/cpu.max";
        if (fs::exists(mpath)) {
            cg_write(mpath, std::to_string(quota_us) + " 100000");
        }
    }

    if (mem_limit_bytes > 0) {
        std::string mpath = path + "/memory.max";
        if (fs::exists(mpath)) {
            cg_write(mpath, std::to_string(mem_limit_bytes));
        }
    }

    if (max_pids > 0) {
        std::string ppath = path + "/pids.max";
        if (fs::exists(ppath)) {
            cg_write(ppath, std::to_string(max_pids));
        }
    }

    std::cout << "[cgroup] created/reused " << path
              << " weight=" << cpu_weight
              << " cpu=" << cpu_limit_ms << "ms"
              << " mem=" << mem_limit_bytes
              << " pids=" << max_pids << std::endl;
    return path;
}

std::string CgroupManager::create_judge(const std::string& task_id,
                                         uint32_t cpu_limit_ms,
                                         uint64_t mem_limit_bytes,
                                         int64_t max_pids) {
    // Judge child cgroup: high weight 200 + hard limits
    return create_with_weight("judge_" + task_id, 200, cpu_limit_ms, mem_limit_bytes, max_pids);
}

std::string CgroupManager::init_data_prep() {
    // data_prep is pre-created by ExecStartPre with weight=50.
    // Just verify it exists and return the path.
    std::string dp_path = CG_ROOT + "/data_prep";
    if (!fs::exists(dp_path + "/cgroup.procs")) {
        // Fallback: try to create if ExecStartPre didn't run
        try {
            return create_with_weight("data_prep", 50, 0, 0, 0);
        } catch (const std::exception& e) {
            std::cerr << "[cgroup] init_data_prep fallback failed: " << e.what() << std::endl;
            return dp_path; // return path anyway, enter_data_prep will handle gracefully
        }
    }
    std::cout << "[cgroup] data_prep already exists at " << dp_path << std::endl;
    return dp_path;
}

void CgroupManager::enter_data_prep() {
    std::string dp_path = CG_ROOT + "/data_prep";
    std::string procs_file = dp_path + "/cgroup.procs";
    if (!fs::exists(procs_file)) {
        std::cerr << "[cgroup] WARNING: " << procs_file << " not found, skipping enter_data_prep" << std::endl;
        return;
    }
    try {
        cg_write(procs_file, std::to_string(getpid()));
        std::cout << "[cgroup] entered data_prep (pid=" << getpid() << ")" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[cgroup] WARNING: enter_data_prep failed: " << e.what() << std::endl;
    }
}

void CgroupManager::attach_pid(const std::string& cg_path, pid_t pid) {
    cg_write(cg_path + "/cgroup.procs", std::to_string(pid));
}

CgroupManager::Usage CgroupManager::read_usage(const std::string& cg_path) {
    Usage u{};
    {
        std::ifstream f(cg_path + "/cpu.stat");
        std::string key; uint64_t val;
        while (f >> key >> val) {
            if (key == "usage_usec") { u.cpu_ns = val * 1000; break; }
        }
    }
    {
        std::ifstream f(cg_path + "/memory.peak");
        if (f.is_open() && (f >> u.mem_bytes)) { /* ok */ }
        else {
            std::ifstream f2(cg_path + "/memory.current");
            if (f2.is_open()) f2 >> u.mem_bytes;
        }
    }
    return u;
}

void CgroupManager::destroy(const std::string& cg_path) {
    std::error_code ec;
    fs::remove_all(cg_path, ec);
    if (ec) std::cerr << "[cgroup] warning: destroy " << cg_path << ": " << ec.message() << std::endl;
}

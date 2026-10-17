// main.cpp —— C++ 中间层 HTTP 网关（生产级）
// 部署本机，唯一持有 MySQL 凭据，覆盖 OJ 全功能接口。
// 特性：统一鉴权（token）、限流、prepared statement 防注入、配置外置。

#include "config.h"
#include "db.h"
#include "lsm_client.h"
#include "crypto.h"
#include "business.h"
#include "rate_limiter.h"
#include "board_cache.h"
#include <json.hpp>
#include <httplib.h>

#include <iostream>
#include <string>
#include <memory>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <mutex>
#include <utility>

using namespace mw;
using json = nlohmann::json;

std::unique_ptr<DBPool> g_db;
std::unique_ptr<LsmClient> g_lsm;
Config g_cfg;
RateLimiter g_limiter;
std::mutex g_db_mtx;  // DB 连接非线程安全，串行化写操作
std::mutex g_lsm_mtx; // LSM 连接非线程安全，串行化
bool g_lsm_up = false;
BoardCache g_board;   // 内存实时榜（每场比赛一张）

// ---- 工具 ----
static std::string hex_rnd(int bytes) {
    static const char* hexc = "0123456789abcdef";
    std::string out;
    std::random_device rd;
    for (int i = 0; i < bytes; ++i) out += hexc[rd() % 16];
    return out;
}

static std::string jerr(const std::string& msg) {
    return (json{{"ok", false}, {"error", msg}}).dump();
}

// 读取一场比赛的 startTime（从 contests.content JSON），返回 "YYYY-MM-DD HH:MM:SS"
static std::string contest_start_time(int cid) {
    auto q = g_db->query("SELECT content FROM contests WHERE id=" + std::to_string(cid));
    if (q.rows.empty()) return "";
    try {
        json c = json::parse(q.rows[0][0]);
        std::string t = c.value("startTime", "");
        if (t.size() == 16) t += ":00";  // "YYYY-MM-DD HH:MM" -> 补齐秒
        return t;
    } catch (...) { return ""; }
}

// 内存榜罚时基准：虚拟=报名时间，正式=比赛开始时间
static std::string board_base_time(int cid, long long uid, bool is_virtual) {
    if (!is_virtual) return contest_start_time(cid);
    auto q = g_db->query("SELECT DATE_FORMAT(registered_at,'%Y-%m-%d %H:%i:%s') "
                         "FROM contest_registrations WHERE user_id=" + std::to_string(uid) +
                         " AND contest_id=" + std::to_string(cid));
    if (!q.rows.empty() && !q.rows[0][0].empty()) return q.rows[0][0];
    return contest_start_time(cid);  // 无报名记录回退比赛开始
}
static std::string jok(const json& data) {
    json o = {{"ok", true}};
    for (auto& [k, v] : data.items()) o[k] = v;
    return o.dump();
}

// 从请求中取 token（header 或 query）
static std::string get_token(const httplib::Request& req) {
    if (req.has_header("Authorization")) {
        std::string a = req.get_header_value("Authorization");
        if (a.rfind("Bearer ", 0) == 0) return a.substr(7);
        return a;
    }
    return req.get_param_value("token");
}

// token 鉴权：返回 user_id，失败抛异常
static long long auth(const httplib::Request& req) {
    std::string token = get_token(req);
    if (token.empty()) throw std::runtime_error("missing token");
    auto q = g_db->query("SELECT user_id,role FROM sessions s JOIN users u ON u.id=s.user_id "
                         "WHERE s.token='" + g_db->esc(token) + "' AND s.expires_at > NOW()");
    if (q.rows.empty()) throw std::runtime_error("invalid or expired token");
    return atoll(q.rows[0][0].c_str());
}

// 鉴权并返回角色（需要角色判断的接口用）
static std::pair<long long, std::string> auth_role(const httplib::Request& req) {
    std::string token = get_token(req);
    if (token.empty()) throw std::runtime_error("missing token");
    auto q = g_db->query("SELECT user_id,role FROM sessions s JOIN users u ON u.id=s.user_id "
                         "WHERE s.token='" + g_db->esc(token) + "' AND s.expires_at > NOW()");
    if (q.rows.empty()) throw std::runtime_error("invalid or expired token");
    return {atoll(q.rows[0][0].c_str()), q.rows[0][1]};
}

// 仅 admin 可访问
static long long auth_admin(const httplib::Request& req) {
    auto [uid, role] = auth_role(req);
    if (role != "admin") throw std::runtime_error("admin role required");
    return uid;
}

// admin 或 author 可访问（出题端写接口）
static long long auth_author(const httplib::Request& req) {
    auto [uid, role] = auth_role(req);
    if (role != "admin" && role != "author") throw std::runtime_error("admin or author role required");
    return uid;
}

// LSM 断线重连（调用方需持有 g_lsm_mtx）
static bool lsm_ready() {
    if (g_lsm_up) return true;
    g_lsm_up = g_lsm->connect(g_cfg.lsm_host, g_cfg.lsm_port);
    return g_lsm_up;
}

// 限流中间件（按 IP）
static bool rate_limit(const httplib::Request& req, httplib::Response& res) {
    std::string ip = req.remote_addr.empty() ? "unknown" : req.remote_addr;
    if (!g_limiter.allow(ip)) {
        res.status = 429;
        res.set_content(jerr("rate limit exceeded"), "application/json");
        return false;
    }
    return true;
}

// 统一异常包装
template <typename F>
static void wrap(const httplib::Request& req, httplib::Response& res, F fn) {
    try {
        fn();
    } catch (const DBError& e) {
        res.status = 500;
        res.set_content(jerr(std::string("db: ") + e.what()), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(jerr(e.what()), "application/json");
    }
}

int main(int argc, char** argv) {
    std::string cfg_path = "config.json";
    if (argc > 1) cfg_path = argv[1];
    g_cfg = Config::load(cfg_path);
    g_limiter = RateLimiter(g_cfg.rate_limit_max, g_cfg.rate_limit_window);

    try {
        g_db = std::make_unique<DBPool>(g_cfg.mysql_host, g_cfg.mysql_port, g_cfg.mysql_user, g_cfg.mysql_pass, g_cfg.mysql_db);
        std::cout << "[init] MySQL 已连接 " << g_cfg.mysql_host << ":" << g_cfg.mysql_port << "/" << g_cfg.mysql_db << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[init] MySQL 连接失败: " << e.what() << "\n";
        return 1;
    }
    g_lsm = std::make_unique<LsmClient>();
    g_lsm_up = g_lsm->connect(g_cfg.lsm_host, g_cfg.lsm_port);
    if (g_lsm_up)
        std::cout << "[init] LSM 已连接 " << g_cfg.lsm_host << ":" << g_cfg.lsm_port << "\n";
    else
        std::cerr << "[init] LSM 连接失败（继续运行）\n";

    // 支持 HTTPS（需编译时定义 CPPHTTPLIB_OPENSSL_SUPPORT）
    std::unique_ptr<httplib::Server> svr;
    if (g_cfg.use_https && !g_cfg.cert_path.empty() && !g_cfg.key_path.empty()) {
        svr = std::make_unique<httplib::SSLServer>(g_cfg.cert_path.c_str(), g_cfg.key_path.c_str());
        std::cout << "[init] 已启用 HTTPS 模式\n";
    } else {
        svr = std::make_unique<httplib::Server>();
    }

    // 全局限流
    svr->set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        return rate_limit(req, res) ? httplib::Server::HandlerResponse::Unhandled
                                    : httplib::Server::HandlerResponse::Handled;
    });

    // ============ 公共接口（无需鉴权）============
    svr->Get("/ping", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(jok({{"pong", true}}), "application/json");
    });

    svr->Post("/api/register", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string username = b.value("username", "");
            std::string password = b.value("password", "");
            std::string role = b.value("role", "user");
            if (username.empty() || password.empty()) throw std::runtime_error("username/password required");
            std::string salt = hex_rnd(16);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            g_db->exec("INSERT INTO users(username,password_hash,salt,role,nickname) VALUES('"
                + g_db->esc(username) + "',SHA2(CONCAT('" + g_db->esc(salt) + "','" + g_db->esc(password) + "'),256),'"
                + g_db->esc(salt) + "','" + g_db->esc(role) + "','" + g_db->esc(username) + "')");
            res.set_content(jok({{"registered", true}}), "application/json");
        });
    });

    svr->Post("/api/login", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string username = b.value("username", "");
            std::string password = b.value("password", "");
            if (username.empty() || password.empty()) throw std::runtime_error("username/password required");
            auto q = g_db->query("SELECT id,salt,password_hash,role,nickname,avatar FROM users WHERE username='" + g_db->esc(username) + "'");
            if (q.rows.empty()) throw std::runtime_error("用户不存在");
            auto& r = q.rows[0];
            long long uid = atoll(r[0].c_str());
            std::string salt = r[1], hash = r[2], role = r[3], nick = r[4], avatar = r[5];
            auto chk = g_db->query("SELECT SHA2(CONCAT('" + g_db->esc(salt) + "','" + g_db->esc(password) + "'),256)");
            if (chk.rows.empty() || chk.rows[0][0] != hash) throw std::runtime_error("密码错误");
            std::string token = hex_rnd(32);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            g_db->exec("REPLACE INTO sessions(token,user_id,expires_at) VALUES('" + token + "',"
                + std::to_string(uid) + ",DATE_ADD(NOW(), INTERVAL 7 DAY))");
            res.set_content(jok({{"token", token}, {"userId", uid}, {"role", role}, {"username", username}, {"nickname", nick}, {"avatar", avatar}}), "application/json");
        });
    });

    // ============ 鉴权接口 ============
    svr->Get("/api/whoami", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            long long uid = auth(req);
            auto q = g_db->query("SELECT username,role,nickname,avatar FROM users WHERE id=" + std::to_string(uid));
            if (q.rows.empty()) throw std::runtime_error("user not found");
            res.set_content(jok({{"userId", uid}, {"username", q.rows[0][0]}, {"role", q.rows[0][1]}, {"nickname", q.rows[0][2]}, {"avatar", q.rows[0][3]}}), "application/json");
        });
    });

    svr->Post("/api/logout", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            std::string token = get_token(req);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            g_db->exec("DELETE FROM sessions WHERE token='" + g_db->esc(token) + "'");
            res.set_content(jok({{"logout", true}}), "application/json");
        });
    });

    // 提交判题结果（需鉴权）
    svr->Post("/api/submit", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            long long uid = auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            int problem_id = b.value("problemId", 0);
            int contest_id = b.value("contestId", 0);
            std::string verdict = b.value("verdict", "");
            std::string detail = b.value("detail", "");
            int time_ms = b.value("timeMs", 0);
            bool virt = b.value("virtual", false);
            if (problem_id <= 0) throw std::runtime_error("problemId required");
            // 报名即获得参赛权限：比赛提交要求已报名，并以报名身份（打星/正式）为准
            if (contest_id > 0) {
                auto reg = contest_registration(uid, contest_id);
                if (!reg.first) throw std::runtime_error("未报名，无法提交比赛题目");
                virt = reg.second;
            }
            std::string ts = b.value("ts", "");
            if (ts.empty()) {
                auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                std::tm tm{}; localtime_r(&t, &tm);
                char buf[32]; snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                                       tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                ts = buf;
            }
            std::lock_guard<std::mutex> lk(g_db_mtx);
            long long sid = upsert_submission(uid, problem_id, contest_id, verdict, detail, time_ms, virt, ts);
            // 更新内存实时榜（比赛提交才上榜）
            if (contest_id > 0) {
                auto uq = g_db->query("SELECT username FROM users WHERE id=" + std::to_string(uid));
                std::string uname = uq.rows.empty() ? std::to_string(uid) : uq.rows[0][0];
                std::string base = board_base_time(contest_id, uid, virt);
                g_board.on_submit(uid, uname, contest_id, problem_id, verdict, ts, virt, base);
            }
            res.set_content(jok({{"sid", sid}}), "application/json");
        });
    });

    // 榜单（公开）
    svr->Get("/api/board", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            int cid = std::stoi(req.get_param_value("cid"));
            std::string start = req.get_param_value("start");
            std::string problems = req.get_param_value("problems");
            auto [official, vrt] = board(cid, start, problems);
            json resp = {{"official", json::parse(official)}, {"virtual", json::parse(vrt)}};
            res.set_content(resp.dump(), "application/json");
        });
    });

    // 实时榜单（内存，微秒级，前端轮询用）
    svr->Get("/api/board/realtime", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            int cid = std::stoi(req.get_param_value("cid"));
            int top = req.has_param("top") ? std::stoi(req.get_param_value("top")) : 100;
            if (top <= 0) top = 100;
            res.set_content(g_board.get(cid, top).dump(), "application/json");
        });
    });

    // 题目（公开读取）
    svr->Get("/api/problems", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() { res.set_content(list_problems(), "application/json"); });
    });
    svr->Get("/api/problem", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            int id = std::stoi(req.get_param_value("id"));
            res.set_content(get_problem(id), "application/json");
        });
    });
    svr->Get("/api/problem_visibility", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() { res.set_content(problem_visibility(), "application/json"); });
    });

    // ============ 出题端接口（需鉴权 + author/admin 角色）============
    svr->Post("/api/problem/upsert", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            upsert_problem(b.value("id", 0), b.value("title", ""), b.value("description", ""),
                           b.value("sampleIn", ""), b.value("sampleOut", ""), b.value("timeMs", 1000),
                           b.value("memMb", 256), b.value("tags", "[]"), b.value("stdCode", ""),
                           b.value("isPublic", true));
            res.set_content(jok({{"saved", true}}), "application/json");
        });
    });

    svr->Get("/api/generators", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() { res.set_content(list_generators(), "application/json"); });
    });
    svr->Post("/api/generator/create", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            long long id = create_generator(b.value("name", ""), b.value("code", ""), b.value("description", ""));
            if (id <= 0) throw std::runtime_error("创建失败（名字可能已存在）");
            res.set_content(jok({{"id", id}}), "application/json");
        });
    });
    svr->Post("/api/generator/update", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            update_generator(b.value("id", 0), b.value("code", ""), b.value("description", ""));
            res.set_content(jok({{"updated", true}}), "application/json");
        });
    });
    svr->Post("/api/generator/delete", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            delete_generator(b.value("id", 0));
            res.set_content(jok({{"deleted", true}}), "application/json");
        });
    });
    svr->Get("/api/generator", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            int id = std::stoi(req.get_param_value("id"));
            res.set_content(get_generator(id), "application/json");
        });
    });
    svr->Get("/api/generator/search", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            std::string kw = req.get_param_value("q");
            res.set_content(search_generators(kw), "application/json");
        });
    });
    svr->Post("/api/generator/bind", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            bind_generator(b.value("problemId", 0), b.value("generatorId", 0), b.value("genCount", 10));
            res.set_content(jok({{"bound", true}}), "application/json");
        });
    });
    svr->Post("/api/generator/unbind", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            unbind_generator(b.value("problemId", 0), b.value("generatorId", 0));
            res.set_content(jok({{"unbound", true}}), "application/json");
        });
    });

    // 测试样例
    svr->Post("/api/testcase/set", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            set_testcase(b.value("problemId", 0), b.value("name", ""), b.value("input", ""), b.value("output", ""), b.value("isSample", false));
            res.set_content(jok({{"saved", true}}), "application/json");
        });
    });
    svr->Get("/api/testcases", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            int pid = std::stoi(req.get_param_value("problemId"));
            res.set_content(list_testcases(pid), "application/json");
        });
    });
    svr->Post("/api/testcase/remove", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            remove_testcase(b.value("problemId", 0), b.value("name", ""));
            res.set_content(jok({{"removed", true}}), "application/json");
        });
    });

    // 比赛
    svr->Post("/api/contest/upsert", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_author(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            upsert_contest(b.value("id", 0), b.value("content", "{}"));
            res.set_content(jok({{"saved", true}}), "application/json");
        });
    });
    svr->Post("/api/contest/register", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            long long uid = auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            contest_register(uid, b.value("contestId", 0), b.value("virtual", false));
            res.set_content(jok({{"registered", true}}), "application/json");
        });
    });
    svr->Get("/api/contest/submissions", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            int cid = std::stoi(req.get_param_value("cid"));
            std::string username = req.get_param_value("username");
            bool view_all = req.get_param_value("view_all") == "1";
            res.set_content(contest_submissions(cid, username, view_all), "application/json");
        });
    });
    svr->Get("/api/progress", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            std::string username = req.get_param_value("username");
            int cid = std::stoi(req.get_param_value("cid"));
            res.set_content(user_progress(username, cid), "application/json");
        });
    });

    // 用户
    svr->Get("/api/users", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth_admin(req);  // 仅 admin
            res.set_content(list_users(), "application/json");
        });
    });
    svr->Post("/api/profile", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            long long uid = auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::lock_guard<std::mutex> lk(g_db_mtx);
            update_profile(uid, b.value("nickname", ""), b.value("avatar", ""));
            res.set_content(jok({{"updated", true}}), "application/json");
        });
    });
    // 按用户名查 user_id（客户端 migration 用）
    svr->Get("/api/user_id", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            std::string username = req.get_param_value("username");
            long long uid = user_id_by_name(username);
            res.set_content(jok({{"userId", uid}}), "application/json");
        });
    });

    // 比赛列表/详情（公开读取，客户端同步用）
    svr->Get("/api/contests", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() { res.set_content(list_contests(), "application/json"); });
    });
    svr->Get("/api/contest", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            int id = std::stoi(req.get_param_value("id"));
            res.set_content(get_contest(id), "application/json");
        });
    });

    // ============ LSM 透传（需鉴权；客户端不再直连 6380）============
    svr->Post("/api/lsm/set", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string k = b.value("key", "");
            if (k.empty()) throw std::runtime_error("key required");
            std::lock_guard<std::mutex> lk(g_lsm_mtx);
            if (!lsm_ready()) throw std::runtime_error("lsm unavailable");
            if (!g_lsm->set(k, b.value("value", ""))) { g_lsm_up = false; throw std::runtime_error("lsm set failed"); }
            res.set_content(jok({{"set", true}}), "application/json");
        });
    });
    svr->Post("/api/lsm/get", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string k = b.value("key", "");
            if (k.empty()) throw std::runtime_error("key required");
            std::lock_guard<std::mutex> lk(g_lsm_mtx);
            if (!lsm_ready()) throw std::runtime_error("lsm unavailable");
            auto v = g_lsm->get(k);
            if (!v) res.set_content(jok({{"found", false}}), "application/json");
            else res.set_content(jok({{"found", true}, {"value", *v}}), "application/json");
        });
    });
    svr->Post("/api/lsm/del", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string k = b.value("key", "");
            if (k.empty()) throw std::runtime_error("key required");
            std::lock_guard<std::mutex> lk(g_lsm_mtx);
            if (!lsm_ready()) throw std::runtime_error("lsm unavailable");
            res.set_content(jok({{"count", g_lsm->del(k)}}), "application/json");
        });
    });
    svr->Post("/api/lsm/incr", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string k = b.value("key", "");
            if (k.empty()) throw std::runtime_error("key required");
            std::lock_guard<std::mutex> lk(g_lsm_mtx);
            if (!lsm_ready()) throw std::runtime_error("lsm unavailable");
            long long v = g_lsm->incr(k);
            if (v <= 0) g_lsm_up = false;
            res.set_content(jok({{"value", v}}), "application/json");
        });
    });
    svr->Post("/api/lsm/hset", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string k = b.value("key", ""), f = b.value("field", "");
            if (k.empty() || f.empty()) throw std::runtime_error("key/field required");
            std::lock_guard<std::mutex> lk(g_lsm_mtx);
            if (!lsm_ready()) throw std::runtime_error("lsm unavailable");
            if (!g_lsm->hset(k, f, b.value("value", ""))) { g_lsm_up = false; throw std::runtime_error("lsm hset failed"); }
            res.set_content(jok({{"set", true}}), "application/json");
        });
    });
    svr->Post("/api/lsm/hget", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string k = b.value("key", ""), f = b.value("field", "");
            if (k.empty() || f.empty()) throw std::runtime_error("key/field required");
            std::lock_guard<std::mutex> lk(g_lsm_mtx);
            if (!lsm_ready()) throw std::runtime_error("lsm unavailable");
            auto v = g_lsm->hget(k, f);
            if (!v) res.set_content(jok({{"found", false}}), "application/json");
            else res.set_content(jok({{"found", true}, {"value", *v}}), "application/json");
        });
    });
    svr->Post("/api/lsm/hkeys", [](const httplib::Request& req, httplib::Response& res) {
        wrap(req, res, [&]() {
            auth(req);
            json b = json::parse(req.body.empty() ? "{}" : req.body);
            std::string k = b.value("key", "");
            if (k.empty()) throw std::runtime_error("key required");
            std::lock_guard<std::mutex> lk(g_lsm_mtx);
            if (!lsm_ready()) throw std::runtime_error("lsm unavailable");
            res.set_content(jok({{"keys", g_lsm->hkeys(k)}}), "application/json");
        });
    });

    // 启动时从 MySQL 重建内存榜（遍历所有有提交的比赛）
    try {
        auto cids = g_db->query("SELECT DISTINCT contest_id FROM submissions WHERE contest_id > 0");
        for (auto& r : cids.rows) {
            int cid = atoi(r[0].c_str());
            if (cid <= 0) continue;
            std::string start = contest_start_time(cid);
            g_board.rebuild(*g_db, cid, start);
        }
        std::cout << "[init] 内存榜已从 MySQL 重建\n";
    } catch (const std::exception& e) {
        std::cerr << "[init] 内存榜重建失败: " << e.what() << "\n";
    }

    std::string proto = g_cfg.use_https ? "https" : "http";
    std::cout << "[init] 中间层监听 0.0.0.0:" << g_cfg.http_port << " (" << proto << ")\n";
    if (!svr->listen("0.0.0.0", g_cfg.http_port)) {
        std::cerr << "监听失败\n";
        return 1;
    }
    return 0;
}

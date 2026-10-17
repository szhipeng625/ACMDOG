// mysql_dao_http.cpp —— 客户端 HTTP 中间层模式的参考实现（模板）
//
// 这是 ojcore 切到中间层模式的改造模板：在原有的 mysql_dao.cpp 基础上，
// 每个 mysql_* 函数增加「HTTP 分支」。当 middlewareUrl 配置非空时走 HTTP，
// 否则走原 libmysql 直连（向后兼容，可灰度切换）。
//
// 用法：
//   1. 把本文件的函数体作为「HTTP 分支」合入 mysql_dao.cpp 各对应函数
//   2. 或在 mysql_dao.cpp 顶部 #include 本文件 + 定义 g_mw_* 全局状态
//
// 中间层地址 + 登录 token 存为全局：
//   oj::g_mw_url   = "http://47.253.41.10:8899"  （从 mysql_config.json 读 middlewareUrl）
//   oj::g_mw_token = "<登录后获得的 token>"
//
// 依赖：http_client.h + http_json.h

#include "http_client.h"
#include "http_json.h"

namespace oj {

// ---- 全局状态（合入 mysql_dao.cpp 时与 g_conn 并列）----
static std::string g_mw_url;          // 中间层地址（空 = 未启用 HTTP 模式）
static std::string g_mw_token;        // 会话 token

inline bool use_middleware() { return !g_mw_url.empty(); }

// ================= 鉴权 =================

bool mw_register(const std::string& username, const std::string& password,
                 const std::string& role, std::string& err) {
    std::string body = "{\"username\":\"" + username + "\",\"password\":\"" + password + "\",\"role\":\"" + role + "\"}";
    std::string r = HttpClient::post(g_mw_url + "/api/register", body);
    if (!jsonOk(r)) { err = jsonError(r); return false; }
    return true;
}

bool mw_login(const std::string& username, const std::string& password,
              std::string& out_token, long long& out_user_id,
              std::string& out_role, std::string& out_username,
              std::string& out_nickname, std::string& out_avatar, std::string& err) {
    std::string body = "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}";
    std::string r = HttpClient::post(g_mw_url + "/api/login", body);
    if (!jsonOk(r)) { err = jsonError(r); return false; }
    out_token = jsonGetStr(r, "token");
    out_user_id = jsonGetInt(r, "userId");
    out_role = jsonGetStr(r, "role");
    out_username = jsonGetStr(r, "username");
    out_nickname = jsonGetStr(r, "nickname");
    g_mw_token = out_token;  // 记住 token，后续请求带
    return true;
}

bool mw_whoami(const std::string& token, long long& out_user_id,
               std::string& out_role, std::string& out_username,
               std::string& out_nickname, std::string& out_avatar, std::string& err) {
    std::string r = HttpClient::get(g_mw_url + "/api/whoami", token);
    if (!jsonOk(r)) { err = jsonError(r); return false; }
    out_user_id = jsonGetInt(r, "userId");
    out_role = jsonGetStr(r, "role");
    out_username = jsonGetStr(r, "username");
    out_nickname = jsonGetStr(r, "nickname");
    return true;
}

bool mw_logout(const std::string& token) {
    HttpClient::post(g_mw_url + "/api/logout", "{}", token);
    g_mw_token.clear();
    return true;
}

// ================= 提交 =================

long long mw_upsert_submission(long long user_id, int problem_id, int contest_id,
                               const std::string& verdict, const std::string& detail,
                               int time_ms, bool virt, const std::string& ts) {
    std::string body = "{\"problemId\":" + std::to_string(problem_id)
        + ",\"contestId\":" + std::to_string(contest_id)
        + ",\"verdict\":\"" + verdict + "\""
        + ",\"detail\":\"" + detail + "\""
        + ",\"timeMs\":" + std::to_string(time_ms)
        + ",\"virtual\":" + (virt ? "true" : "false")
        + ",\"ts\":\"" + ts + "\"}";
    std::string r = HttpClient::post(g_mw_url + "/api/submit", body, g_mw_token);
    if (!jsonOk(r)) return 0;
    return jsonGetInt(r, "sid");
}

// ================= 榜单 =================

bool mw_board(int cid, const std::string& start_str, const std::string& problems_csv,
              std::string& out_official, std::string& out_virtual) {
    std::string url = g_mw_url + "/api/board?cid=" + std::to_string(cid)
        + "&start=" + start_str + "&problems=" + problems_csv;
    std::string r = HttpClient::get(url);
    if (r.empty()) return false;
    // 中间层 board 返回 {"official":[...],"virtual":[...]}
    // 直接透传给上层（上层原期望两个 JSON 数组字符串）
    out_official = r;
    out_virtual = "";
    return true;
}

// ================= 题目 =================

bool mw_list_problems(std::string& out_json) {
    out_json = HttpClient::get(g_mw_url + "/api/problems");
    return !out_json.empty();
}

bool mw_get_problem(int id, std::string& out_json) {
    out_json = HttpClient::get(g_mw_url + "/api/problem?id=" + std::to_string(id));
    return !out_json.empty();
}

bool mw_upsert_problem(int id, const std::string& title, const std::string& desc,
                       const std::string& sample_in, const std::string& sample_out,
                       int time_ms, int mem_mb, const std::string& tags_json,
                       const std::string& std_code, bool is_public) {
    std::string body = "{\"id\":" + std::to_string(id)
        + ",\"title\":\"" + title + "\""
        + ",\"description\":\"" + desc + "\""
        + ",\"sampleIn\":\"" + sample_in + "\""
        + ",\"sampleOut\":\"" + sample_out + "\""
        + ",\"timeMs\":" + std::to_string(time_ms)
        + ",\"memMb\":" + std::to_string(mem_mb)
        + ",\"tags\":\"" + tags_json + "\""
        + ",\"stdCode\":\"" + std_code + "\""
        + ",\"isPublic\":" + (is_public ? "true" : "false") + "}";
    std::string r = HttpClient::post(g_mw_url + "/api/problem/upsert", body, g_mw_token);
    return jsonOk(r);
}

// ================= 生成器 =================

bool mw_list_generators(std::string& out_json) {
    out_json = HttpClient::get(g_mw_url + "/api/generators");
    return !out_json.empty();
}

long long mw_create_generator(const std::string& name, const std::string& code,
                              const std::string& desc) {
    std::string body = "{\"name\":\"" + name + "\",\"code\":\"" + code + "\",\"description\":\"" + desc + "\"}";
    std::string r = HttpClient::post(g_mw_url + "/api/generator/create", body, g_mw_token);
    if (!jsonOk(r)) return 0;
    return jsonGetInt(r, "id");
}

bool mw_bind_generator(int problem_id, int generator_id, int gen_count) {
    std::string body = "{\"problemId\":" + std::to_string(problem_id)
        + ",\"generatorId\":" + std::to_string(generator_id)
        + ",\"genCount\":" + std::to_string(gen_count) + "}";
    std::string r = HttpClient::post(g_mw_url + "/api/generator/bind", body, g_mw_token);
    return jsonOk(r);
}

// ================= 比赛 =================

bool mw_contest_register(long long user_id, int contest_id, bool virt) {
    std::string body = "{\"contestId\":" + std::to_string(contest_id)
        + ",\"virtual\":" + (virt ? "true" : "false") + "}";
    std::string r = HttpClient::post(g_mw_url + "/api/contest/register", body, g_mw_token);
    return jsonOk(r);
}

bool mw_contest_submissions(int cid, const std::string& username, bool view_all, std::string& out_json) {
    std::string url = g_mw_url + "/api/contest/submissions?cid=" + std::to_string(cid)
        + "&username=" + username + (view_all ? "&view_all=1" : "");
    std::string r = HttpClient::get(url, g_mw_token);
    if (r.empty()) return false;
    out_json = r;
    return true;
}

bool mw_user_progress(const std::string& username, int contest_id, std::string& out_json) {
    std::string url = g_mw_url + "/api/progress?username=" + username + "&cid=" + std::to_string(contest_id);
    std::string r = HttpClient::get(url);
    if (r.empty()) return false;
    out_json = r;
    return true;
}

bool mw_user_id_by_name(const std::string& username, long long& out_user_id) {
    std::string r = HttpClient::get(g_mw_url + "/api/user_id?username=" + username);
    if (!jsonOk(r)) return false;
    out_user_id = jsonGetInt(r, "userId");
    return out_user_id > 0;
}

} // namespace oj

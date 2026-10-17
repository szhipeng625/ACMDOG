// business.cpp —— 核心业务实现（复刻 ojcore/mysql_dao.cpp 全量逻辑）
#include "config.h"
#include "db.h"
#include "lsm_client.h"
#include "crypto.h"
#include "business.h"
#include <json.hpp>

#include <map>
#include <vector>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <memory>

using namespace mw;
using json = nlohmann::json;

extern std::unique_ptr<DBPool> g_db;
extern std::unique_ptr<LsmClient> g_lsm;

// ============ 提交落库（ICPC 语义）============
long long upsert_submission(long long user_id, int problem_id, int contest_id,
                            const std::string& verdict, const std::string& detail,
                            int time_ms, bool virt, const std::string& ts) {
    int cid = contest_id > 0 ? contest_id : 0;
    bool ac = (verdict == "AC");

    std::string curVerdict;
    long long existingId = 0;
    bool exists = false;
    auto q = g_db->query("SELECT id,verdict FROM submissions WHERE user_id="
                         + std::to_string(user_id) + " AND problem_id=" + std::to_string(problem_id)
                         + " AND contest_id=" + std::to_string(cid) + " LIMIT 1");
    if (!q.rows.empty()) {
        exists = true;
        existingId = atoll(q.rows[0][0].c_str());
        curVerdict = q.rows[0][1];
    }

    if (exists && curVerdict == "AC") return existingId;

    if (!exists) {
        long long wrong = ac ? 0 : 1;
        g_db->exec("INSERT INTO submissions(user_id,problem_id,contest_id,verdict,detail,time_ms,`virtual`,wrong_count,created_at) VALUES("
                   + std::to_string(user_id) + "," + std::to_string(problem_id) + ","
                   + std::to_string(cid) + ",'" + g_db->esc(verdict) + "','" + g_db->esc(detail)
                   + "'," + std::to_string(time_ms) + "," + (virt ? "1" : "0") + ","
                   + std::to_string(wrong) + ",'" + g_db->esc(ts) + "')");
        return g_db->last_id();
    }
    if (ac) {
        g_db->exec("UPDATE submissions SET verdict='AC', detail='" + g_db->esc(detail)
                   + "', time_ms=" + std::to_string(time_ms)
                   + ", `virtual`=" + (virt ? "1" : "0")
                   + ", created_at='" + g_db->esc(ts) + "'"
                   + " WHERE user_id=" + std::to_string(user_id)
                   + " AND problem_id=" + std::to_string(problem_id)
                   + " AND contest_id=" + std::to_string(cid));
        return existingId;
    }
    g_db->exec("UPDATE submissions SET verdict='" + g_db->esc(verdict)
               + "', detail='" + g_db->esc(detail) + "', time_ms=" + std::to_string(time_ms)
               + ", `virtual`=" + (virt ? "1" : "0")
               + ", wrong_count=wrong_count+1"
               + " WHERE user_id=" + std::to_string(user_id)
               + " AND problem_id=" + std::to_string(problem_id)
               + " AND contest_id=" + std::to_string(cid));
    return existingId;
}

// ============ ICPC 榜单 ============
std::pair<std::string, std::string> board(int cid, const std::string& start_str,
                                          const std::string& problems_csv) {
    std::string startEsc = g_db->esc(start_str);
    std::string sql =
        "SELECT u.username, s.`virtual`, s.wrong_count, "
        "  TIMESTAMPDIFF(MINUTE, IF(s.`virtual`=1, COALESCE(cr.registered_at, '" + startEsc + "'), '" + startEsc + "'), s.created_at) AS rel_min, "
        "  IF(s.verdict='AC',1,0) AS is_ac "
        "FROM submissions s JOIN users u ON u.id=s.user_id "
        "LEFT JOIN contest_registrations cr ON cr.user_id=s.user_id AND cr.contest_id=s.contest_id "
        "WHERE s.contest_id=" + std::to_string(cid) +
        " AND s.problem_id IN (" + problems_csv + ")";
    auto q = g_db->query(sql);
    std::map<std::string, long long> solved, penalty;
    std::map<std::string, bool> isVirt;
    for (auto& row : q.rows) {
        std::string username = row[0];
        int virt = atoi(row[1].c_str());
        long long wrong = atoll(row[2].c_str());
        long long relMin = atoll(row[3].c_str());
        bool ac = atoi(row[4].c_str()) != 0;
        if (!ac) continue;
        solved[username] += 1;
        penalty[username] += relMin + wrong * 20;
        isVirt[username] = (virt == 1);
    }
    auto emit = [&](bool virt) -> std::string {
        struct O { std::string name; long long s, p; };
        std::vector<O> v;
        for (auto& kv : solved) if (isVirt[kv.first] == virt) v.push_back({kv.first, kv.second, penalty[kv.first]});
        std::sort(v.begin(), v.end(), [](const O& a, const O& b) {
            if (a.s != b.s) return a.s > b.s; return a.p < b.p;
        });
        json out = json::array();
        for (size_t i = 0; i < v.size(); ++i)
            out.push_back({{"rank", (int)i + 1}, {"username", v[i].name}, {"solved", v[i].s}, {"penalty", v[i].p}});
        return out.dump();
    };
    return {emit(false), emit(true)};
}

// ============ 用户 ============
std::string list_users() {
    auto q = g_db->query("SELECT id,username,role,created_at FROM users ORDER BY id");
    json arr = json::array();
    for (auto& r : q.rows)
        arr.push_back({{"id", atoll(r[0].c_str())}, {"username", r[1]}, {"role", r[2]}, {"createdAt", r[3]}});
    return arr.dump();
}

std::string update_profile(long long user_id, const std::string& nickname, const std::string& avatar) {
    g_db->exec("UPDATE users SET nickname='" + g_db->esc(nickname) + "', avatar='" + g_db->esc(avatar) + "' WHERE id=" + std::to_string(user_id));
    return "{\"ok\":true}";
}

long long user_id_by_name(const std::string& username) {
    auto q = g_db->query("SELECT id FROM users WHERE username='" + g_db->esc(username) + "' LIMIT 1");
    return q.rows.empty() ? 0 : atoll(q.rows[0][0].c_str());
}

std::string user_progress(const std::string& username, int contest_id) {
    long long uid = user_id_by_name(username);
    if (uid <= 0) return "[]";
    auto q = g_db->query("SELECT problem_id,verdict FROM submissions WHERE user_id="
                         + std::to_string(uid) + " AND contest_id=" + std::to_string(contest_id)
                         + " ORDER BY problem_id");
    json arr = json::array();
    for (auto& r : q.rows) {
        bool ac = (r[1] == "AC");
        arr.push_back({{"problemId", atoi(r[0].c_str())}, {"verdict", r[1]}, {"ac", ac}});
    }
    return arr.dump();
}

// ============ 题目 ============
std::string list_problems() {
    auto q = g_db->query(
        "SELECT p.id, p.title, p.is_public, "
        "COALESCE((SELECT SUM(pg.gen_count) FROM problem_generators pg WHERE pg.problem_id=p.id),0) "
        "FROM problems p ORDER BY p.id");
    json arr = json::array();
    for (auto& r : q.rows)
        arr.push_back({{"id", atoi(r[0].c_str())}, {"title", r[1]}, {"isPublic", atoi(r[2].c_str()) != 0}, {"dataCount", atoi(r[3].c_str())}});
    return arr.dump();
}

std::string problem_visibility() {
    auto q = g_db->query("SELECT id,is_public FROM problems ORDER BY id");
    json obj = json::object();
    for (auto& r : q.rows) obj[r[0]] = (atoi(r[1].c_str()) != 0);
    return obj.dump();
}

bool upsert_problem(int id, const std::string& title, const std::string& desc,
                    const std::string& sample_in, const std::string& sample_out,
                    int time_ms, int mem_mb, const std::string& tags_json,
                    const std::string& std_code, bool is_public) {
    if (time_ms <= 0) time_ms = 1000;
    if (mem_mb <= 0) mem_mb = 256;
    std::string tags = tags_json.empty() ? "[]" : tags_json;
    std::string encStd = std_code.empty() ? "" : encrypt_blob(std_code);
    g_db->exec("INSERT INTO problems(id,title,description,sample_in,sample_out,time_ms,mem_mb,tags,std_code,is_public,updated_at) VALUES("
        + std::to_string(id) + ",'" + g_db->esc(title) + "','" + g_db->esc(desc) + "','"
        + g_db->esc(sample_in) + "','" + g_db->esc(sample_out) + "'," + std::to_string(time_ms) + ","
        + std::to_string(mem_mb) + ",'" + g_db->esc(tags) + "','" + g_db->esc(encStd) + "',"
        + (is_public ? "1" : "0") + ",NOW()) "
        "ON DUPLICATE KEY UPDATE title=VALUES(title), description=VALUES(description), "
        "sample_in=VALUES(sample_in), sample_out=VALUES(sample_out), time_ms=VALUES(time_ms), "
        "mem_mb=VALUES(mem_mb), tags=VALUES(tags), std_code=VALUES(std_code), "
        "is_public=VALUES(is_public), updated_at=NOW()");
    return true;
}

std::string get_problem(int id) {
    auto q = g_db->query("SELECT title,description,sample_in,sample_out,time_ms,mem_mb,tags,std_code,is_public,updated_at FROM problems WHERE id=" + std::to_string(id));
    if (q.rows.empty()) return "{\"ok\":false,\"error\":\"题目不存在\"}";
    auto& r = q.rows[0];
    std::string tags = r[6].empty() ? "[]" : r[6];
    std::string stdCode = decrypt_blob(r[7]);
    json out = {
        {"ok", true}, {"id", id}, {"title", r[0]}, {"description", r[1]},
        {"sampleIn", r[2]}, {"sampleOut", r[3]}, {"timeMs", atoi(r[4].c_str())},
        {"memMb", atoi(r[5].c_str())}, {"tags", json::parse(tags.empty() ? "[]" : tags)},
        {"stdCode", stdCode}, {"isPublic", atoi(r[8].c_str()) != 0}, {"updatedAt", r[9]},
    };
    // 关联生成器
    auto gq = g_db->query("SELECT g.id,g.name,g.description,g.code,pg.gen_count FROM problem_generators pg JOIN generators g ON g.id=pg.generator_id WHERE pg.problem_id=" + std::to_string(id) + " ORDER BY g.name");
    json gens = json::array();
    for (auto& gr : gq.rows)
        gens.push_back({{"id", atoi(gr[0].c_str())}, {"name", gr[1]}, {"description", gr[2]},
                        {"code", decrypt_blob(gr[3])}, {"genCount", atoi(gr[4].c_str())}});
    out["generators"] = gens;
    return out.dump();
}

// ============ 生成器 ============
std::string list_generators() {
    auto q = g_db->query("SELECT id,name,description,created_at FROM generators ORDER BY name");
    json arr = json::array();
    for (auto& r : q.rows)
        arr.push_back({{"id", atoi(r[0].c_str())}, {"name", r[1]}, {"description", r[2]}, {"createdAt", r[3]}});
    return arr.dump();
}

std::string get_generator(int id) {
    auto q = g_db->query("SELECT name,code,description FROM generators WHERE id=" + std::to_string(id));
    if (q.rows.empty()) return "{\"ok\":false}";
    auto& r = q.rows[0];
    return (json{{"ok", true}, {"id", id}, {"name", r[0]}, {"code", decrypt_blob(r[1])}, {"description", r[2]}}).dump();
}

long long create_generator(const std::string& name, const std::string& code, const std::string& desc) {
    std::string enc = encrypt_blob(code);
    try {
        g_db->exec("INSERT INTO generators(name,code,description) VALUES('"
                   + g_db->esc(name) + "','" + g_db->esc(enc) + "','" + g_db->esc(desc) + "')");
    } catch (const DBError&) { return 0; }  // 重名等
    return (long long)g_db->last_id();
}

bool update_generator(int id, const std::string& code, const std::string& desc) {
    std::string enc = encrypt_blob(code);
    g_db->exec("UPDATE generators SET code='" + g_db->esc(enc) + "', description='" + g_db->esc(desc) + "' WHERE id=" + std::to_string(id));
    return true;
}

bool delete_generator(int id) {
    // 删除生成器及其与所有题目的绑定关系
    g_db->exec("DELETE FROM problem_generators WHERE generator_id=" + std::to_string(id));
    g_db->exec("DELETE FROM generators WHERE id=" + std::to_string(id));
    return true;
}

std::string problem_generators(int problem_id) {
    auto q = g_db->query("SELECT g.id,g.name,pg.gen_count FROM problem_generators pg JOIN generators g ON g.id=pg.generator_id WHERE pg.problem_id=" + std::to_string(problem_id) + " ORDER BY g.name");
    json arr = json::array();
    for (auto& r : q.rows)
        arr.push_back({{"id", atoi(r[0].c_str())}, {"name", r[1]}, {"genCount", atoi(r[2].c_str())}});
    return arr.dump();
}

bool bind_generator(int problem_id, int generator_id, int gen_count) {
    if (gen_count <= 0) gen_count = 10;
    auto q = g_db->query("SELECT name FROM generators WHERE id=" + std::to_string(generator_id));
    if (q.rows.empty()) return false;
    std::string name = q.rows[0][0];
    unsigned long long gh = hash_string(name);
    int seedBase = (int)(((unsigned long long)problem_id * 1000003ull + gh) & 0x7fffffff);
    g_db->exec("INSERT INTO problem_generators(problem_id,generator_id,gen_count,seed_base) VALUES("
        + std::to_string(problem_id) + "," + std::to_string(generator_id) + ","
        + std::to_string(gen_count) + "," + std::to_string(seedBase) + ") "
        "ON DUPLICATE KEY UPDATE gen_count=VALUES(gen_count), seed_base=VALUES(seed_base)");
    return true;
}

bool unbind_generator(int problem_id, int generator_id) {
    g_db->exec("DELETE FROM problem_generators WHERE problem_id=" + std::to_string(problem_id)
               + " AND generator_id=" + std::to_string(generator_id));
    return true;
}

std::string search_generators(const std::string& keyword) {
    if (keyword.empty()) return "[]";
    auto q = g_db->query("SELECT id,name,code FROM generators ORDER BY name");
    std::string kw = keyword;
    for (auto& c : kw) c = (char)tolower((unsigned char)c);
    json arr = json::array();
    for (auto& r : q.rows) {
        int id = atoi(r[0].c_str());
        std::string name = r[1];
        std::string code = decrypt_blob(r[2]);
        std::istringstream iss(code);
        std::string line; int lineNo = 0, n = 0;
        while (std::getline(iss, line) && n < 30) {
            ++lineNo;
            std::string ll = line;
            for (auto& c : ll) c = (char)tolower((unsigned char)c);
            if (ll.find(kw) != std::string::npos) {
                std::string t = line;
                size_t a = t.find_first_not_of(" \t\r\n");
                size_t b = t.find_last_not_of(" \t\r\n");
                if (a != std::string::npos) t = t.substr(a);
                if (b != std::string::npos && b+1 < t.size()) t = t.substr(0, b+1);
                arr.push_back({{"id", id}, {"name", name}, {"lineNo", lineNo}, {"line", t}, {"keyword", keyword}});
                ++n;
            }
        }
    }
    return arr.dump();
}

long long add_problem_generator(int problem_id, const std::string& name,
                                const std::string& code, const std::string& desc,
                                int gen_count, int seed_base) {
    std::string encCode = encrypt_blob(code);
    g_db->exec("INSERT INTO generators(code,description) VALUES('" + g_db->esc(encCode) + "','" + g_db->esc(desc) + "')");
    long long gid = (long long)g_db->last_id();
    if (gid <= 0) return 0;
    // 注意：ojcore 此函数 SQL 含 name 列，但 schema 的 problem_generators 无 name 列；
    // 这里以 schema 为准（problem_id,generator_id,gen_count,seed_base）
    g_db->exec("INSERT INTO problem_generators(problem_id,generator_id,gen_count,seed_base) VALUES("
        + std::to_string(problem_id) + "," + std::to_string(gid) + ","
        + std::to_string(gen_count) + "," + std::to_string(seed_base) + ")");
    return gid;
}

bool clear_problem_generators(int problem_id) {
    g_db->exec("DELETE FROM problem_generators WHERE problem_id=" + std::to_string(problem_id));
    return true;
}

void purge_orphan_generators() {
    try { g_db->exec("DELETE FROM generators WHERE id NOT IN (SELECT generator_id FROM problem_generators)"); }
    catch (...) {}
}

// ============ 测试样例 ============
bool set_testcase(int problem_id, const std::string& name, const std::string& input,
                  const std::string& output, bool is_sample) {
    g_db->exec("INSERT INTO problem_testcases(problem_id,name,input,output,is_sample) VALUES("
        + std::to_string(problem_id) + ",'" + g_db->esc(name) + "','" + g_db->esc(input)
        + "','" + g_db->esc(output) + "'," + (is_sample ? "1" : "0") + ") "
        "ON DUPLICATE KEY UPDATE input=VALUES(input), output=VALUES(output), is_sample=VALUES(is_sample)");
    return true;
}
bool remove_testcase(int problem_id, const std::string& name) {
    g_db->exec("DELETE FROM problem_testcases WHERE problem_id=" + std::to_string(problem_id)
               + " AND name='" + g_db->esc(name) + "'");
    return true;
}
std::string list_testcases(int problem_id) {
    auto q = g_db->query("SELECT name,input,output,is_sample FROM problem_testcases WHERE problem_id=" + std::to_string(problem_id) + " ORDER BY sort,id");
    json arr = json::array();
    for (auto& r : q.rows)
        arr.push_back({{"name", r[0]}, {"input", r[1]}, {"output", r[2]}, {"isSample", atoi(r[3].c_str()) != 0}});
    return arr.dump();
}

// ============ 比赛 ============
bool upsert_contest(int cid, const std::string& contest_json) {
    g_db->exec("INSERT INTO contests(id,content) VALUES(" + std::to_string(cid) + ",'" + g_db->esc(contest_json) + "') "
               "ON DUPLICATE KEY UPDATE content=VALUES(content)");
    return true;
}
bool contest_register(long long user_id, int contest_id, bool virt) {
    g_db->exec("INSERT INTO contest_registrations(user_id,contest_id,is_virtual) VALUES("
        + std::to_string(user_id) + "," + std::to_string(contest_id) + "," + (virt ? "1" : "0") + ") "
        "ON DUPLICATE KEY UPDATE is_virtual=VALUES(is_virtual)");
    return true;
}
std::pair<bool,bool> contest_registration(long long user_id, int contest_id) {
    auto q = g_db->query("SELECT is_virtual FROM contest_registrations WHERE user_id="
                         + std::to_string(user_id) + " AND contest_id=" + std::to_string(contest_id) + " LIMIT 1");
    if (q.rows.empty()) return {false, false};
    return {true, atoi(q.rows[0][0].c_str()) != 0};
}
std::string contest_submissions(int cid, const std::string& username, bool view_all) {
    std::string sql =
        "SELECT s.id,s.problem_id,u.username,s.verdict,s.detail,s.time_ms,s.`virtual`,DATE_FORMAT(s.created_at,'%Y-%m-%d %H:%i:%s') "
        "FROM submissions s JOIN users u ON u.id=s.user_id WHERE s.contest_id=" + std::to_string(cid);
    if (!view_all) sql += " AND u.username='" + g_db->esc(username) + "'";
    sql += " ORDER BY s.created_at DESC, s.id DESC";
    auto q = g_db->query(sql);
    json arr = json::array();
    for (auto& r : q.rows)
        arr.push_back({{"id", atoll(r[0].c_str())}, {"problemId", atoi(r[1].c_str())}, {"username", r[2]},
                       {"verdict", r[3]}, {"detail", r[4]}, {"timeMs", atoi(r[5].c_str())},
                       {"virtual", atoi(r[6].c_str()) != 0}, {"ts", r[7]}});
    return arr.dump();
}

std::string list_contests() {
    auto q = g_db->query("SELECT id, content FROM contests ORDER BY id");
    json arr = json::array();
    for (auto& r : q.rows)
        arr.push_back({{"id", atoi(r[0].c_str())}, {"content", r[1]}});
    return arr.dump();
}

std::string get_contest(int id) {
    auto q = g_db->query("SELECT content FROM contests WHERE id=" + std::to_string(id));
    if (q.rows.empty()) return "{\"ok\":false,\"error\":\"比赛不存在\"}";
    return (json{{"ok", true}, {"id", id}, {"content", q.rows[0][0]}}).dump();
}

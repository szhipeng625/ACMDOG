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
#include <cstdio>
#include <sstream>
#include <memory>

using namespace mw;
using json = nlohmann::json;

extern std::unique_ptr<DBPool> g_db;
extern std::unique_ptr<LsmClient> g_lsm;

// ============ 提交落库（ICPC 语义：同一用户同一题原地更新，AC 后锁定）============
long long upsert_submission(long long user_id, int problem_id, int contest_id,
                            const std::string& verdict, const std::string& detail,
                            int time_ms, bool virt, const std::string& ts,
                            const std::string& code) {
    int cid = contest_id > 0 ? contest_id : 0;
    bool ac = (verdict == "AC");
    std::string where = " WHERE user_id=" + std::to_string(user_id)
        + " AND problem_id=" + std::to_string(problem_id)
        + " AND contest_id=" + std::to_string(cid);

    std::string curVerdict;
    long long existingId = 0;
    bool exists = false;
    auto q = g_db->query("SELECT id,verdict FROM submissions" + where + " LIMIT 1");
    if (!q.rows.empty()) {
        exists = true;
        existingId = atoll(q.rows[0][0].c_str());
        curVerdict = q.rows[0][1];
    }

    // 已 AC 则锁定：非 AC 提交不覆盖 AC 代码；但 AC→AC 允许更新代码
    //（例如旧记录 code 为 NULL，或用户改进了 AC 代码）。
    if (exists && curVerdict == "AC" && !ac) return existingId;

    if (!exists) {
        long long wrong = ac ? 0 : 1;
        g_db->exec("INSERT INTO submissions(user_id,problem_id,contest_id,verdict,detail,time_ms,`virtual`,wrong_count,created_at,code) VALUES("
                   + std::to_string(user_id) + "," + std::to_string(problem_id) + ","
                   + std::to_string(cid) + ",'" + g_db->esc(verdict) + "','" + g_db->esc(detail)
                   + "'," + std::to_string(time_ms) + "," + (virt ? "1" : "0") + ","
                   + std::to_string(wrong) + ",'" + g_db->esc(ts) + "','" + g_db->esc(code) + "')");
        return g_db->last_id();
    }
    if (ac) {
        g_db->exec("UPDATE submissions SET verdict='AC', detail='" + g_db->esc(detail)
                   + "', time_ms=" + std::to_string(time_ms)
                   + ", `virtual`=" + (virt ? "1" : "0")
                   + ", created_at='" + g_db->esc(ts) + "', code='" + g_db->esc(code) + "'"
                   + where);
        return existingId;
    }
    g_db->exec("UPDATE submissions SET verdict='" + g_db->esc(verdict)
               + "', detail='" + g_db->esc(detail) + "', time_ms=" + std::to_string(time_ms)
               + ", `virtual`=" + (virt ? "1" : "0")
               + ", wrong_count=wrong_count+1"
               + ", code='" + g_db->esc(code) + "'"
               + where);
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

// 某用户某题（contest_id=0 练习）最近一次提交（含代码），用于恢复编辑器 / AC 代码维护
std::string user_solution(long long uid, int problem_id, int contest_id) {
    int cid = contest_id > 0 ? contest_id : 0;
    auto q = g_db->query("SELECT id,verdict,IFNULL(detail,''),IFNULL(code,''),DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s') "
                         "FROM submissions WHERE user_id=" + std::to_string(uid)
                         + " AND problem_id=" + std::to_string(problem_id)
                         + " AND contest_id=" + std::to_string(cid) + " ORDER BY id DESC LIMIT 1");
    if (q.rows.empty()) return "{\"found\":false}";
    json o = {{"found", true}, {"id", atoll(q.rows[0][0].c_str())}, {"verdict", q.rows[0][1]},
              {"detail", q.rows[0][2]}, {"code", q.rows[0][3]}, {"ts", q.rows[0][4]}};
    return o.dump();
}

// ============ 题目 ============
std::string list_problems() {
    auto q = g_db->query(
        "SELECT p.id, p.title, p.is_public, p.time_ms, p.mem_mb, p.tags, p.judge_mode, "
        "COALESCE((SELECT SUM(pg.gen_count) FROM problem_generators pg WHERE pg.problem_id=p.id),0) "
        "FROM problems p ORDER BY p.id");
    json arr = json::array();
    for (auto& r : q.rows) {
        json tagArr = json::array();
        std::string tags = r[5].empty() ? "[]" : r[5];
        try { tagArr = json::parse(tags); } catch (...) { tagArr = json::array(); }
        arr.push_back({{"id", atoi(r[0].c_str())}, {"title", r[1]}, {"isPublic", atoi(r[2].c_str()) != 0},
                       {"timeMs", atoi(r[3].c_str())}, {"memMb", atoi(r[4].c_str())},
                       {"tags", tagArr}, {"judgeMode", atoi(r[6].c_str()) != 0},
                       {"dataCount", atoi(r[7].c_str())}});
    }
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
                    const std::string& std_code, const std::string& std_exe,
                    const std::string& spj_code, const std::string& spj_exe,
                    const std::string& entry_code, const std::string& entry_exe,
                    const std::string& itf_template, int judge_mode,
                    bool is_public) {
    if (time_ms <= 0) time_ms = 1000;
    if (mem_mb <= 0) mem_mb = 256;
    std::string tags = tags_json.empty() ? "[]" : tags_json;
    std::string encStd = std_code.empty() ? "" : encrypt_blob(std_code);
    std::string encStdExe = std_exe.empty() ? "" : encrypt_blob(base64_decode(std_exe));
    std::string encSpj = spj_code.empty() ? "" : encrypt_blob(spj_code);
    std::string encSpjExe = spj_exe.empty() ? "" : encrypt_blob(base64_decode(spj_exe));
    std::string encEntry = entry_code.empty() ? "" : encrypt_blob(entry_code);
    std::string encEntryExe = entry_exe.empty() ? "" : encrypt_blob(base64_decode(entry_exe));
    std::string encItf = itf_template.empty() ? "" : encrypt_blob(itf_template);
    g_db->exec("INSERT INTO problems(id,title,description,sample_in,sample_out,time_ms,mem_mb,tags,std_code,std_exe,spj_code,spj_exe,entry_code,entry_exe,itf_template,judge_mode,is_public,updated_at) VALUES("
        + std::to_string(id) + ",'" + g_db->esc(title) + "','" + g_db->esc(desc) + "','"
        + g_db->esc(sample_in) + "','" + g_db->esc(sample_out) + "'," + std::to_string(time_ms) + ","
        + std::to_string(mem_mb) + ",'" + g_db->esc(tags) + "','" + g_db->esc(encStd) + "','"
        + g_db->esc(encStdExe) + "','" + g_db->esc(encSpj) + "','" + g_db->esc(encSpjExe) + "','"
        + g_db->esc(encEntry) + "','" + g_db->esc(encEntryExe) + "','" + g_db->esc(encItf) + "',"
        + std::to_string(judge_mode ? 1 : 0) + "," + (is_public ? "1" : "0") + ",NOW()) "
        "ON DUPLICATE KEY UPDATE title=VALUES(title), description=VALUES(description), "
        "sample_in=VALUES(sample_in), sample_out=VALUES(sample_out), time_ms=VALUES(time_ms), "
        "mem_mb=VALUES(mem_mb), tags=VALUES(tags), std_code=VALUES(std_code), std_exe=VALUES(std_exe), "
        "spj_code=VALUES(spj_code), spj_exe=VALUES(spj_exe), "
        "entry_code=VALUES(entry_code), entry_exe=VALUES(entry_exe), "
        "itf_template=VALUES(itf_template), judge_mode=VALUES(judge_mode), "
        "is_public=VALUES(is_public), updated_at=NOW()");
    return true;
}

std::string get_problem(int id) {
    auto q = g_db->query("SELECT title,description,sample_in,sample_out,time_ms,mem_mb,tags,std_code,std_exe,spj_code,spj_exe,entry_code,entry_exe,itf_template,judge_mode,is_public,updated_at FROM problems WHERE id=" + std::to_string(id));
    if (q.rows.empty()) return "{\"ok\":false,\"error\":\"题目不存在\"}";
    auto& r = q.rows[0];
    std::string tags = r[6].empty() ? "[]" : r[6];
    std::string stdCode = decrypt_blob(r[7]);
    std::string stdExe = r[8].empty() ? "" : base64_encode(decrypt_blob(r[8]));
    std::string spjCode = decrypt_blob(r[9]);
    std::string spjExe = r[10].empty() ? "" : base64_encode(decrypt_blob(r[10]));
    std::string entryCode = decrypt_blob(r[11]);
    std::string entryExe = r[12].empty() ? "" : base64_encode(decrypt_blob(r[12]));
    std::string itfTemplate = decrypt_blob(r[13]);
    json out = {
        {"ok", true}, {"id", id}, {"title", r[0]}, {"description", r[1]},
        {"sampleIn", r[2]}, {"sampleOut", r[3]}, {"timeMs", atoi(r[4].c_str())},
        {"memMb", atoi(r[5].c_str())}, {"tags", json::parse(tags.empty() ? "[]" : tags)},
        {"stdCode", stdCode}, {"stdExe", stdExe}, {"spjCode", spjCode}, {"spjExe", spjExe},
        {"entryCode", entryCode}, {"entryExe", entryExe}, {"itfTemplate", itfTemplate},
        {"judgeMode", atoi(r[14].c_str())}, {"isPublic", atoi(r[15].c_str()) != 0}, {"updatedAt", r[16]},
    };
    // 关联生成器
    auto gq = g_db->query("SELECT g.id,g.name,g.description,g.code,g.exe,pg.gen_count FROM problem_generators pg JOIN generators g ON g.id=pg.generator_id WHERE pg.problem_id=" + std::to_string(id) + " ORDER BY g.name");
    json gens = json::array();
    for (auto& gr : gq.rows)
        gens.push_back({{"id", atoi(gr[0].c_str())}, {"name", gr[1]}, {"description", gr[2]},
                        {"code", decrypt_blob(gr[3])}, {"exe", gr[4].empty() ? "" : base64_encode(decrypt_blob(gr[4]))},
                        {"genCount", atoi(gr[5].c_str())}});
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

bool update_generator_exe(int id, const std::string& exe_base64) {
    std::string enc = exe_base64.empty() ? "" : encrypt_blob(base64_decode(exe_base64));
    g_db->exec("UPDATE generators SET exe='" + g_db->esc(enc) + "' WHERE id=" + std::to_string(id));
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
// ============ ACMDOGCodex：运行用量 + 题面改动审计 ============

// 数值入参做一次钳位：这些值来自客户端，虽然走的是本端鉴权接口，
// 但拼进 SQL 前仍然不能放任（cost_usd 尤其——它是字符串拼进语句的）。
static long long clamp_ll(long long v, long long lo, long long hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void codex_record_run(const std::string& run_uid, const std::string& username,
                      const std::string& model, const std::string& effort,
                      const std::string& instruction, int tool_calls, long long elapsed_ms,
                      bool ok, const std::string& error,
                      long long in_tok, long long out_tok,
                      long long cache_read, long long cache_write,
                      long long context_tok, double cost_usd) {
    if (run_uid.empty()) return;
    if (!(cost_usd >= 0.0) || cost_usd > 100000.0) cost_usd = 0.0;   // 顺手挡住 NaN / inf

    char cost[32];
    snprintf(cost, sizeof(cost), "%.6f", cost_usd);

    // REPLACE 而非 INSERT：同一 run_uid 重传以最后一次为准，客户端重试不留重复行
    g_db->exec("REPLACE INTO codex_runs(run_uid,username,model,effort,instruction,tool_calls,"
               "elapsed_ms,ok,error,input_tokens,output_tokens,cache_read_tokens,cache_write_tokens,"
               "context_tokens,cost_usd,ts) VALUES('"
        + g_db->esc(run_uid) + "','" + g_db->esc(username) + "','" + g_db->esc(model) + "','"
        + g_db->esc(effort) + "','" + g_db->esc(instruction) + "',"
        + std::to_string(clamp_ll(tool_calls, 0, 100000)) + ","
        + std::to_string(clamp_ll(elapsed_ms, 0, 86400000LL)) + ","
        + (ok ? "1" : "0") + ",'" + g_db->esc(error) + "',"
        + std::to_string(clamp_ll(in_tok, 0, 1000000000LL)) + ","
        + std::to_string(clamp_ll(out_tok, 0, 1000000000LL)) + ","
        + std::to_string(clamp_ll(cache_read, 0, 1000000000LL)) + ","
        + std::to_string(clamp_ll(cache_write, 0, 1000000000LL)) + ","
        + std::to_string(clamp_ll(context_tok, 0, 1000000000LL)) + ","
        + std::string(cost) + ",NOW())");
}

void codex_record_audit(const std::string& run_uid, int problem_id, const std::string& username,
                        const std::string& before_title, const std::string& before_desc,
                        const std::string& after_title, const std::string& after_desc,
                        const std::string& tags_csv, int time_ms, int mem_mb,
                        const std::string& message) {
    if (run_uid.empty() || problem_id <= 0) return;

    g_db->exec("INSERT INTO codex_statement_audits(run_uid,problem_id,username,"
               "before_title,before_description,after_title,after_description,"
               "tags,time_ms,mem_mb,message,ts) VALUES('"
        + g_db->esc(run_uid) + "'," + std::to_string(problem_id) + ",'" + g_db->esc(username) + "','"
        + g_db->esc(before_title) + "','" + g_db->esc(before_desc) + "','"
        + g_db->esc(after_title) + "','" + g_db->esc(after_desc) + "','"
        + g_db->esc(tags_csv) + "',"
        + std::to_string(clamp_ll(time_ms, 0, 1000000)) + ","
        + std::to_string(clamp_ll(mem_mb, 0, 1000000)) + ",'"
        + g_db->esc(message) + "',NOW())");
}

std::string codex_list_runs(int limit, int problem_id) {
    limit = (int)clamp_ll(limit, 1, 500);

    // problem_id > 0 时只列「改动过该题」的运行 —— 题目页上看「这题被 AI 改过几次」最常用
    std::string sql = "SELECT r.run_uid,r.username,r.model,r.effort,r.instruction,r.tool_calls,"
                      "r.elapsed_ms,r.ok,r.error,r.input_tokens,r.output_tokens,"
                      "r.cache_read_tokens,r.cache_write_tokens,r.context_tokens,r.cost_usd,r.ts";
    if (problem_id > 0) {
        sql += ",(SELECT COUNT(*) FROM codex_statement_audits a WHERE a.run_uid=r.run_uid"
               " AND a.problem_id=" + std::to_string(problem_id) + ") AS audits";
        sql += " FROM codex_runs r WHERE EXISTS(SELECT 1 FROM codex_statement_audits a"
               " WHERE a.run_uid=r.run_uid AND a.problem_id=" + std::to_string(problem_id) + ")";
    } else {
        sql += ",(SELECT COUNT(*) FROM codex_statement_audits a WHERE a.run_uid=r.run_uid) AS audits";
        sql += " FROM codex_runs r";
    }
    sql += " ORDER BY r.id DESC LIMIT " + std::to_string(limit);

    auto q = g_db->query(sql);
    json arr = json::array();
    for (auto& r : q.rows) {
        arr.push_back({
            {"runUid", r[0]}, {"username", r[1]}, {"model", r[2]}, {"effort", r[3]},
            {"instruction", r[4]}, {"toolCalls", atoi(r[5].c_str())},
            {"elapsedMs", atoll(r[6].c_str())}, {"ok", atoi(r[7].c_str()) != 0},
            {"error", r[8]},
            {"inputTokens", atoll(r[9].c_str())}, {"outputTokens", atoll(r[10].c_str())},
            {"cacheReadTokens", atoll(r[11].c_str())}, {"cacheWriteTokens", atoll(r[12].c_str())},
            {"contextTokens", atoll(r[13].c_str())}, {"costUsd", atof(r[14].c_str())},
            {"ts", r[15]}, {"auditCount", atoi(r[16].c_str())}
        });
    }
    return arr.dump();
}

std::string codex_list_audits(const std::string& run_uid, int problem_id, int limit) {
    limit = (int)clamp_ll(limit, 1, 200);

    std::string where = " WHERE 1=1";
    if (!run_uid.empty()) where += " AND run_uid='" + g_db->esc(run_uid) + "'";
    if (problem_id > 0)  where += " AND problem_id=" + std::to_string(problem_id);

    auto q = g_db->query("SELECT id,run_uid,problem_id,username,before_title,before_description,"
                         "after_title,after_description,tags,time_ms,mem_mb,message,ts"
                         " FROM codex_statement_audits" + where +
                         " ORDER BY id DESC LIMIT " + std::to_string(limit));

    json arr = json::array();
    for (auto& r : q.rows) {
        arr.push_back({
            {"id", atoll(r[0].c_str())}, {"runUid", r[1]}, {"problemId", atoi(r[2].c_str())},
            {"username", r[3]},
            {"beforeTitle", r[4]}, {"beforeDescription", r[5]},
            {"afterTitle", r[6]}, {"afterDescription", r[7]},
            {"tags", r[8]}, {"timeMs", atoi(r[9].c_str())}, {"memMb", atoi(r[10].c_str())},
            {"message", r[11]}, {"ts", r[12]}
        });
    }
    return arr.dump();
}

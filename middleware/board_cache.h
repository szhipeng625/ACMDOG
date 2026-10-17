// board_cache.h —— 内存实时榜单（ICPC，每场比赛一张）
// 实时读走内存（std::sort 微秒级），LSM/MySQL 只做持久化兜底。
// 重启后从 MySQL submissions 表重建。
#pragma once

#include <json.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <ctime>
#include <cstdlib>

#include "db.h"

namespace mw {

using json = nlohmann::json;

struct ProbState {
    int wrong = 0;   // AC 前的错误提交数
    bool ac = false; // 是否已通过（首个 AC 后锁定）
    int ac_min = 0;  // 首个 AC 相对罚时基准的分钟数
};
struct PlayerState {
    long long user_id = 0;
    std::string username;
    bool is_virtual = false;
    std::string base_time;  // 罚时基准（正式=比赛开始，虚拟=报名时间）
    int solved = 0;
    long long penalty = 0;  // 分钟
    std::unordered_map<int, ProbState> problem_state; // pid -> 每题状态（XCPC 明细）
};

// 每场比赛一张榜：contest_id -> (user_id -> PlayerState)
class BoardCache {
public:
    // 解析 "YYYY-MM-DD HH:MM:SS" 为 time_t
    static time_t parse_ts(const std::string& s) {
        std::tm tm{};
        if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d",
                   &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
        tm.tm_year -= 1900; tm.tm_mon -= 1;
        return mktime(&tm);
    }

    // 两个时间的分钟差（b - a），向上取整
    static int minutes_between(const std::string& a, const std::string& b) {
        time_t ta = parse_ts(a), tb = parse_ts(b);
        if (ta == 0 || tb == 0) return 0;
        long long diff = (long long)(tb - ta);
        if (diff < 0) diff = 0;
        return (int)((diff + 59) / 60);  // 向上取整到分钟
    }

    // 判题结果入榜（ICPC 语义）
    void on_submit(long long uid, const std::string& uname, int cid,
                   int problem_id, const std::string& verdict,
                   const std::string& submit_ts, bool is_virtual,
                   const std::string& base_time) {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        auto& p = boards_[cid][uid];
        p.user_id = uid;
        p.username = uname;
        p.is_virtual = is_virtual;
        p.base_time = base_time;

        auto& st = p.problem_state[problem_id];
        if (st.ac) return;  // AC 锁定，后续提交不改

        int rel_min = minutes_between(base_time, submit_ts);

        if (verdict == "AC") {
            p.solved += 1;
            p.penalty += rel_min + st.wrong * 20;
            st.ac = true;
            st.ac_min = rel_min;
        } else {
            st.wrong += 1;
        }
    }

    // 取一场比赛的榜（official + virtual），各按 AC数降序 → 罚时升序
    json get(int cid, int top_n) {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        auto it = boards_.find(cid);
        if (it == boards_.end()) return json{{"official", json::array()}, {"virtual", json::array()}};

        std::vector<const PlayerState*> off, virt;
        for (auto& [uid, p] : it->second)
            (p.is_virtual ? virt : off).push_back(&p);

        auto cmp = [](const PlayerState* a, const PlayerState* b) {
            if (a->solved != b->solved) return a->solved > b->solved;
            if (a->penalty != b->penalty) return a->penalty < b->penalty;
            return a->username < b->username;
        };
        std::sort(off.begin(), off.end(), cmp);
        std::sort(virt.begin(), virt.end(), cmp);

        auto tojson = [&](const std::vector<const PlayerState*>& vv) {
            json arr = json::array();
            int n = std::min((int)vv.size(), top_n);
            for (int i = 0; i < n; ++i) {
                json probs = json::object();
                for (auto& [pid, st] : vv[i]->problem_state) {
                    if (st.ac)
                        probs[std::to_string(pid)] = {{"state", "passed"}, {"wrong", st.wrong}, {"min", st.ac_min}};
                    else if (st.wrong > 0)
                        probs[std::to_string(pid)] = {{"state", "failed"}, {"wrong", st.wrong}};
                }
                arr.push_back({{"rank", i + 1}, {"username", vv[i]->username},
                               {"solved", vv[i]->solved}, {"penalty", vv[i]->penalty},
                               {"problems", probs}});
            }
            return arr;
        };
        return json{{"official", tojson(off)}, {"virtual", tojson(virt)}};
    }

    // 从 MySQL 重建某场比赛的内存榜
    void rebuild(DBPool& db, int cid, const std::string& contest_start) {
        // 拉该比赛所有提交 + 报名时间
        auto q = db.query(
            "SELECT s.user_id, u.username, s.problem_id, s.verdict, "
            "s.`virtual`, DATE_FORMAT(s.created_at,'%Y-%m-%d %H:%i:%s'), "
            "DATE_FORMAT(cr.registered_at,'%Y-%m-%d %H:%i:%s') "
            "FROM submissions s JOIN users u ON u.id=s.user_id "
            "LEFT JOIN contest_registrations cr ON cr.user_id=s.user_id AND cr.contest_id=s.contest_id "
            "WHERE s.contest_id=" + std::to_string(cid) + " ORDER BY s.id");

        for (auto& r : q.rows) {
            long long uid = atoll(r[0].c_str());
            std::string uname = r[1];
            int pid = atoi(r[2].c_str());
            std::string verdict = r[3];
            bool virt = atoi(r[4].c_str()) != 0;
            std::string created_at = r[5];
            std::string registered_at = r[6];
            // 罚时基准：虚拟=报名时间（无则比赛开始），正式=比赛开始
            std::string base = virt ? (registered_at.empty() ? contest_start : registered_at)
                                    : contest_start;
            // 注意：这里用「提交时刻」相对基准的分钟；但 AC 锁定后 created_at 是首次 AC 时间
            on_submit(uid, uname, cid, pid, verdict, created_at, virt, base);
        }
    }

private:
    std::unordered_map<int, std::unordered_map<long long, PlayerState>> boards_;
    std::shared_mutex mtx_;
};

} // namespace mw

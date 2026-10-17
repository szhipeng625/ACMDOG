// business.h —— 业务层声明（复刻 ojcore/mysql_dao.cpp 全量接口）
#pragma once

#include <string>
#include <vector>
#include <utility>

long long upsert_submission(long long user_id, int problem_id, int contest_id,
                            const std::string& verdict, const std::string& detail,
                            int time_ms, bool virt, const std::string& ts,
                            const std::string& code);
std::pair<std::string, std::string> board(int cid, const std::string& start_str,
                                          const std::string& problems_csv);

// 用户
std::string list_users();
std::string update_profile(long long user_id, const std::string& nickname,
                           const std::string& avatar);
long long user_id_by_name(const std::string& username);
std::string user_progress(const std::string& username, int contest_id);
std::string user_solution(long long uid, int problem_id, int contest_id);

// 题目
std::string list_problems();
std::string problem_visibility();
bool upsert_problem(int id, const std::string& title, const std::string& desc,
                    const std::string& sample_in, const std::string& sample_out,
                    int time_ms, int mem_mb, const std::string& tags_json,
                    const std::string& std_code, const std::string& std_exe,
                    const std::string& spj_code, const std::string& spj_exe,
                    const std::string& entry_code, const std::string& entry_exe,
                    const std::string& itf_template, int judge_mode,
                    bool is_public);
std::string get_problem(int id);

// 生成器
std::string list_generators();
std::string get_generator(int id);
long long create_generator(const std::string& name, const std::string& code, const std::string& desc);
bool update_generator(int id, const std::string& code, const std::string& desc);
bool update_generator_exe(int id, const std::string& exe_base64);
bool delete_generator(int id);
std::string problem_generators(int problem_id);
bool bind_generator(int problem_id, int generator_id, int gen_count);
bool unbind_generator(int problem_id, int generator_id);
std::string search_generators(const std::string& keyword);

// ============ ACMDOGCodex：运行用量 + 题面改动审计 ============
// 出题端的 AI 助手对题库有写权限，这里给它留痕：每次指令一行用量账，
// 每次写回题面一行改动记录（前后对照），用 run_uid 关联。
void codex_record_run(const std::string& run_uid, const std::string& username,
                      const std::string& model, const std::string& effort,
                      const std::string& instruction, int tool_calls, long long elapsed_ms,
                      bool ok, const std::string& error,
                      long long in_tok, long long out_tok,
                      long long cache_read, long long cache_write,
                      long long context_tok, double cost_usd);
void codex_record_audit(const std::string& run_uid, int problem_id, const std::string& username,
                        const std::string& before_title, const std::string& before_desc,
                        const std::string& after_title, const std::string& after_desc,
                        const std::string& tags_csv, int time_ms, int mem_mb,
                        const std::string& message);
// 运行列表（含用量合计）；problem_id > 0 时只列改动过该题的运行
std::string codex_list_runs(int limit, int problem_id);
// 某次运行（或某道题）的题面改动明细
std::string codex_list_audits(const std::string& run_uid, int problem_id, int limit);

long long add_problem_generator(int problem_id, const std::string& name,
                                const std::string& code, const std::string& desc,
                                int gen_count, int seed_base);
bool clear_problem_generators(int problem_id);
void purge_orphan_generators();

// 测试样例
bool set_testcase(int problem_id, const std::string& name,
                  const std::string& input, const std::string& output, bool is_sample);
bool remove_testcase(int problem_id, const std::string& name);
std::string list_testcases(int problem_id);

// 比赛
bool upsert_contest(int cid, const std::string& contest_json);
bool contest_register(long long user_id, int contest_id, bool virt);
std::pair<bool,bool> contest_registration(long long user_id, int contest_id);
std::string contest_submissions(int cid, const std::string& username, bool view_all);
std::string list_contests();
std::string get_contest(int id);

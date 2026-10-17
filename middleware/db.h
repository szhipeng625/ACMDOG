// db.h —— MySQL 数据访问层（prepared statement 防注入）
// 中间层唯一持有 MySQL 凭据的组件，所有 SQL 走参数化。
#pragma once

#include <mysql/mysql.h>

#include <string>
#include <stdexcept>
#include <vector>
#include <memory>
#include <cstring>
#include <map>

namespace mw {

struct DBError : std::runtime_error { DBError(const std::string& m) : std::runtime_error(m) {} };

class DB {
public:
    DB(const std::string& host, int port, const std::string& user,
       const std::string& pass, const std::string& db)
        : host_(host), port_(port), user_(user), pass_(pass), db_(db) {
        connect();
    }
    ~DB() { if (conn_) mysql_close(conn_); }

    // 建立（或重建）连接
    void connect() {
        if (conn_) { mysql_close(conn_); conn_ = nullptr; }
        conn_ = mysql_init(nullptr);
        if (!conn_) throw DBError("mysql_init failed");
        unsigned int timeout = 10;
        mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        if (!mysql_real_connect(conn_, host_.c_str(), user_.c_str(), pass_.c_str(),
                                db_.c_str(), port_, nullptr, 0)) {
            std::string err = mysql_error(conn_);
            mysql_close(conn_);
            conn_ = nullptr;
            throw DBError("connect failed: " + err);
        }
        mysql_set_character_set(conn_, "utf8mb4");
    }

    // 连接保活：若被 MySQL 服务端断开（wait_timeout/网络闪断/服务重启），自动重建。
    void ensure_alive() {
        if (!conn_) { connect(); return; }
        if (mysql_ping(conn_) != 0) connect();
    }

    // 转义字符串（用于非参数化的动态 SQL，如表名/列名已白名单的场景）
    std::string esc(const std::string& s) {
        if (!conn_) return s;
        std::string out(s.size() * 2 + 1, '\0');
        mysql_real_escape_string(conn_, &out[0], s.c_str(), s.size());
        out.resize(strlen(out.c_str()));
        return out;
    }

    // 执行无结果集 SQL（返回受影响行数）
    unsigned long long exec(const std::string& sql) {
        if (mysql_query(conn_, sql.c_str()) != 0)
            throw DBError("query failed: " + std::string(mysql_error(conn_)));
        return mysql_affected_rows(conn_);
    }

    // 执行查询，返回 (列名, 行数据) —— 行数据为 vector<vector<string>>
    struct QueryResult {
        std::vector<std::string> cols;
        std::vector<std::vector<std::string>> rows;
    };
    QueryResult query(const std::string& sql) {
        if (mysql_query(conn_, sql.c_str()) != 0)
            throw DBError("query failed: " + std::string(mysql_error(conn_)));
        MYSQL_RES* res = mysql_store_result(conn_);
        if (!res) return {};
        QueryResult qr;
        unsigned int n = mysql_num_fields(res);
        MYSQL_FIELD* fields = mysql_fetch_fields(res);
        for (unsigned int i = 0; i < n; ++i) qr.cols.push_back(fields[i].name);
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            std::vector<std::string> r;
            unsigned long* lens = mysql_fetch_lengths(res);
            for (unsigned int i = 0; i < n; ++i)
                r.push_back(row[i] ? std::string(row[i], lens[i]) : "");
            qr.rows.push_back(std::move(r));
        }
        mysql_free_result(res);
        return qr;
    }

    // 参数化 INSERT/UPDATE（值走 ? 绑定，防注入）
    unsigned long long insert(const std::string& table,
                              const std::vector<std::string>& cols,
                              const std::vector<std::string>& values) {
        if (cols.size() != values.size()) throw DBError("cols/values size mismatch");
        std::string sql = "INSERT INTO `" + table + "` (";
        std::string ph;
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) { sql += ","; ph += ","; }
            sql += "`" + cols[i] + "`";
            ph += "?";
        }
        sql += ") VALUES (" + ph + ")";
        MYSQL_STMT* stmt = mysql_stmt_init(conn_);
        if (!stmt) throw DBError("stmt init failed");
        if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size()) != 0) {
            std::string e = mysql_stmt_error(stmt); mysql_stmt_close(stmt);
            throw DBError("prepare failed: " + e);
        }
        std::vector<MYSQL_BIND> b(values.size());
        std::vector<unsigned long> len(values.size());
        memset(b.data(), 0, sizeof(MYSQL_BIND) * b.size());
        for (size_t i = 0; i < values.size(); ++i) {
            len[i] = values[i].size();
            b[i].buffer_type = MYSQL_TYPE_STRING;
            b[i].buffer = const_cast<char*>(values[i].c_str());
            b[i].buffer_length = values[i].size();
            b[i].length = &len[i];
        }
        mysql_stmt_bind_param(stmt, b.data());
        if (mysql_stmt_execute(stmt) != 0) {
            std::string e = mysql_stmt_error(stmt); mysql_stmt_close(stmt);
            throw DBError("execute failed: " + e);
        }
        unsigned long long aff = mysql_stmt_affected_rows(stmt);
        mysql_stmt_close(stmt);
        return aff;
    }

    // 取最后插入的自增 id
    unsigned long long last_id() {
        return mysql_insert_id(conn_);
    }

private:
    std::string host_; int port_; std::string user_, pass_, db_;
    MYSQL* conn_ = nullptr;
};

// ============================================================
// DBPool —— 线程本地连接池
// 每个线程一个连接（thread_local），避免每次请求建连/释放，
// 同时避免单连接互斥带来的并发瓶颈。
// 接口与 DB 完全一致，调用方 g_db->query()/exec()/esc() 无需改动。
// ============================================================
class DBPool {
public:
    DBPool(const std::string& host, int port, const std::string& user,
           const std::string& pass, const std::string& db)
        : host_(host), port_(port), user_(user), pass_(pass), db_(db) {
        // 预创建一个连接，验证配置可用（失败抛出）
        get_thread_db();
    }

    // 非拷贝
    DBPool(const DBPool&) = delete;
    DBPool& operator=(const DBPool&) = delete;

    // 取当前线程的连接（不存在则新建；存在则先做保活检查，断线自动重建）
    DB& get_thread_db() {
        if (!tls_db_) {
            tls_db_ = std::make_unique<DB>(host_, port_, user_, pass_, db_);
        } else {
            tls_db_->ensure_alive();
        }
        return *tls_db_;
    }

    // 拼接转义（转发到线程本地连接）
    std::string esc(const std::string& s) { return get_thread_db().esc(s); }
    unsigned long long exec(const std::string& sql) { return get_thread_db().exec(sql); }
    DB::QueryResult query(const std::string& sql) { return get_thread_db().query(sql); }
    unsigned long long insert(const std::string& t, const std::vector<std::string>& c,
                              const std::vector<std::string>& v) { return get_thread_db().insert(t, c, v); }
    // 取最后插入的自增 id。
    // ⚠️ 不能走 get_thread_db()：它会 ensure_alive()→mysql_ping()，而 mysql_ping 会
    // 清空 mysql_insert_id，导致 INSERT 之后取到的 id 恒为 0（AC 代码虽已落库，
    // 但 /api/submit 返回 sid=0）。这里直接读当前线程连接，调用方须已在本线程执行过 INSERT。
    unsigned long long last_id() {
        return tls_db_ ? tls_db_->last_id() : 0;
    }

private:
    std::string host_; int port_; std::string user_, pass_, db_;
    static thread_local std::unique_ptr<DB> tls_db_;
};

inline thread_local std::unique_ptr<DB> DBPool::tls_db_;

} // namespace mw

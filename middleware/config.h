// config.h —— 中间层全局配置（从 JSON 配置文件加载，支持环境变量覆盖）
#pragma once

#include <cstdlib>
#include <string>
#include <fstream>
#include <json.hpp>

namespace mw {

inline std::string envStr(const char* k, const std::string& def) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : def;
}

inline int envInt(const char* k, int def) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::atoi(v) : def;
}

inline bool envBool(const char* k, bool def) {
    const char* v = std::getenv(k);
    if (!v || !*v) return def;
    std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" || s == "yes";
}

struct Config {
    // HTTP / HTTPS
    int http_port = 8899;
    bool use_https = false;
    std::string cert_path;
    std::string key_path;

    // MySQL —— 凭据不内置默认值，必须由 config.json 或环境变量注入
    std::string mysql_host;
    int mysql_port = 3306;
    std::string mysql_user;
    std::string mysql_pass;
    std::string mysql_db;

    // LSM
    std::string lsm_host;
    int lsm_port = 6380;

    // 限流
    int rate_limit_max = 60;
    int rate_limit_window = 60;

    static Config load(const std::string& path) {
        Config c;
        std::ifstream f(path);
        if (f.is_open()) {
            try {
                nlohmann::json j = nlohmann::json::parse(f);
                if (j.contains("http_port")) c.http_port = j["http_port"];
                if (j.contains("use_https")) c.use_https = j["use_https"];
                if (j.contains("cert_path")) c.cert_path = j["cert_path"];
                if (j.contains("key_path")) c.key_path = j["key_path"];
                if (j.contains("mysql")) {
                    auto& m = j["mysql"];
                    if (m.contains("host")) c.mysql_host = m["host"];
                    if (m.contains("port")) c.mysql_port = m["port"];
                    if (m.contains("user")) c.mysql_user = m["user"];
                    if (m.contains("pass")) c.mysql_pass = m["pass"];
                    if (m.contains("db")) c.mysql_db = m["db"];
                }
                if (j.contains("lsm")) {
                    auto& l = j["lsm"];
                    if (l.contains("host")) c.lsm_host = l["host"];
                    if (l.contains("port")) c.lsm_port = l["port"];
                }
                if (j.contains("rate_limit")) {
                    auto& r = j["rate_limit"];
                    if (r.contains("max")) c.rate_limit_max = r["max"];
                    if (r.contains("window")) c.rate_limit_window = r["window"];
                }
            } catch (...) {}
        }

        // 环境变量覆盖（优先级最高，供容器化与 CI 注入）
        c.http_port = envInt("MW_HTTP_PORT", c.http_port);
        c.use_https = envBool("MW_USE_HTTPS", c.use_https);
        c.cert_path = envStr("MW_CERT_PATH", c.cert_path);
        c.key_path = envStr("MW_KEY_PATH", c.key_path);
        c.mysql_host = envStr("MW_MYSQL_HOST", c.mysql_host);
        c.mysql_port = envInt("MW_MYSQL_PORT", c.mysql_port);
        c.mysql_user = envStr("MW_MYSQL_USER", c.mysql_user);
        c.mysql_pass = envStr("MW_MYSQL_PASS", c.mysql_pass);
        c.mysql_db = envStr("MW_MYSQL_DB", c.mysql_db);
        c.lsm_host = envStr("MW_LSM_HOST", c.lsm_host);
        c.lsm_port = envInt("MW_LSM_PORT", c.lsm_port);
        c.rate_limit_max = envInt("MW_RATE_LIMIT_MAX", c.rate_limit_max);
        c.rate_limit_window = envInt("MW_RATE_LIMIT_WINDOW", c.rate_limit_window);
        return c;
    }
};

} // namespace mw

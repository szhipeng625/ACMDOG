// http_json.h —— ojcore 客户端用的极简 JSON 工具
// 中间层返回 JSON，客户端只需解析简单字段（token/userId/字符串等）。
// 不引第三方库，提供最小可用：取字符串字段、取数值字段、判定 ok。
#pragma once

#include <string>
#include <cstdlib>

namespace oj {

// 极简 JSON 字符串字段提取：从 {"key":"value"} 里取 value
// 只处理转义的 \" 和 \\，够中间层返回的简单场景用
inline std::string jsonGetStr(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return "";
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    // 跳过空白
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return "";
    if (json[p] == '"') {  // 字符串
        ++p;
        std::string out;
        while (p < json.size() && json[p] != '"') {
            if (json[p] == '\\' && p + 1 < json.size()) {
                ++p;
                char c = json[p];
                if (c == 'n') out += '\n';
                else if (c == 't') out += '\t';
                else if (c == 'r') out += '\r';
                else if (c == '"') out += '"';
                else if (c == '\\') out += '\\';
                else out += c;
            } else {
                out += json[p];
            }
            ++p;
        }
        return out;
    }
    if (json[p] == '-' || (json[p] >= '0' && json[p] <= '9')) {  // 数字返回字面
        size_t s = p;
        while (p < json.size() && (json[p] == '-' || json[p] == '.' || (json[p] >= '0' && json[p] <= '9'))) ++p;
        return json.substr(s, p - s);
    }
    return "";
}

inline long long jsonGetInt(const std::string& json, const std::string& key) {
    return atoll(jsonGetStr(json, key).c_str());
}

inline bool jsonGetBool(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    p = json.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    return p < json.size() && json[p] == 't';
}

// 判定 {"ok":true} / {"ok":false}
inline bool jsonOk(const std::string& json) {
    return json.find("\"ok\":true") != std::string::npos ||
           json.find("\"ok\": true") != std::string::npos;
}

// 取 error 字段
inline std::string jsonError(const std::string& json) {
    return jsonGetStr(json, "error");
}

} // namespace oj

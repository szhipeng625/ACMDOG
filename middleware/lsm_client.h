// lsm_client.h —— 极简 RESP 客户端（连远端 LSM 6380）
// 只实现中间层需要的命令：PING / SET / GET / DEL / HSET / HGET / HKEYS
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <stdexcept>
#include <cstring>
#include <sstream>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace mw {

struct LsmError : std::runtime_error { LsmError(const std::string& m) : std::runtime_error(m) {} };

class LsmClient {
public:
    LsmClient() = default;
    ~LsmClient() { disconnect(); }

    bool connect(const std::string& host, int port) {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        struct timeval tv{3, 0};
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_); sock_ = -1; return false;
        }
        return true;
    }
    void disconnect() { if (sock_ >= 0) { close(sock_); sock_ = -1; } }

    bool ping() { return cmd0("PING").has_value(); }

    bool set(const std::string& k, const std::string& v) {
        return cmd("SET", {k, v});
    }
    std::optional<std::string> get(const std::string& k) {
        auto r = cmd1("GET", {k}); return r;
    }
    long long del(const std::string& k) {
        auto r = cmd1("DEL", {k});
        return r ? atoll(r->c_str()) : 0;
    }
    long long incr(const std::string& k) {
        auto r = cmd1("INCR", {k});
        return r ? atoll(r->c_str()) : 0;
    }
    bool hset(const std::string& k, const std::string& f, const std::string& v) {
        return cmd("HSET", {k, f, v});
    }
    std::optional<std::string> hget(const std::string& k, const std::string& f) {
        return cmd1("HGET", {k, f});
    }
    std::vector<std::string> hkeys(const std::string& k) {
        std::vector<std::string> keys;
        // RESP: HKEYS key
        std::string req = "*2\r\n$5\r\nHKEYS\r\n$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
        if (!sendRaw(req)) return keys;
        std::string line;
        if (!readLine(line)) return keys;
        if (line[0] == '*') {
            int n = std::stoi(line.substr(1));
            for (int i = 0; i < n; ++i) {
                if (!readLine(line)) break;       // $len
                int len = std::stoi(line.substr(1));
                std::string val;
                if (!readBulk(len, val)) break;
                if (!readLine(line)) break;       // 尾 \r\n（已含在 readBulk? 需处理）
                keys.push_back(val);
            }
        }
        return keys;
    }

private:
    int sock_ = -1;

    bool sendRaw(const std::string& data) {
        if (sock_ < 0) return false;
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t n = send(sock_, data.data() + sent, data.size() - sent, 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }
    bool readLine(std::string& line) {
        line.clear();
        char c;
        while (recv(sock_, &c, 1, 0) == 1) {
            line += c;
            if (line.size() >= 2 && line[line.size()-2] == '\r' && line.back() == '\n') {
                line.pop_back(); line.pop_back();  // 去 \r\n
                return true;
            }
        }
        return false;
    }
    bool readBulk(int len, std::string& out) {
        out.clear();
        out.resize(len);
        size_t got = 0;
        while (got < (size_t)len) {
            ssize_t n = recv(sock_, &out[got], len - got, 0);
            if (n <= 0) return false;
            got += n;
        }
        return true;
    }

    // 发送一条不含值返回的命令（如 PING）
    std::optional<std::string> cmd0(const std::string& name) {
        std::string req = "*1\r\n$" + std::to_string(name.size()) + "\r\n" + name + "\r\n";
        if (!sendRaw(req)) return std::nullopt;
        std::string line;
        if (!readLine(line)) return std::nullopt;
        if (line[0] == '-') return std::nullopt;  // error
        return line;
    }

    // 发送带参数命令，返回 simple/bulk 值
    bool cmd(const std::string& name, const std::vector<std::string>& args) {
        std::string req = "*" + std::to_string(args.size()+1) + "\r\n$"
            + std::to_string(name.size()) + "\r\n" + name + "\r\n";
        for (auto& a : args)
            req += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        if (!sendRaw(req)) return false;
        std::string line;
        if (!readLine(line)) return false;
        if (line[0] == '-') return false;
        return true;
    }

    // 发送带参数命令，返回值（bulk/simple）
    std::optional<std::string> cmd1(const std::string& name, const std::vector<std::string>& args) {
        std::string req = "*" + std::to_string(args.size()+1) + "\r\n$"
            + std::to_string(name.size()) + "\r\n" + name + "\r\n";
        for (auto& a : args)
            req += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        if (!sendRaw(req)) return std::nullopt;
        std::string line;
        if (!readLine(line)) return std::nullopt;
        if (line[0] == '-') return std::nullopt;      // error
        if (line[0] == '$') {                          // bulk
            int len = std::stoi(line.substr(1));
            if (len < 0) return std::nullopt;         // nil
            std::string val;
            if (!readBulk(len, val)) return std::nullopt;
            std::string crlf; readLine(crlf);          // 尾 \r\n
            return val;
        }
        if (line[0] == '+') return line.substr(1);     // simple
        if (line[0] == ':') return line.substr(1);     // integer
        return std::nullopt;
    }
};

} // namespace mw

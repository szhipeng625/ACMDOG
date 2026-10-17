// http_client.h —— ojcore 客户端用的极简 HTTP 客户端（Windows Winsock）
// 用于客户端切到「HTTP 中间层模式」时，替代 libmysql 直连。
// 复用 resp_client.cpp 的 Winsock 手法，同步阻塞式 GET/POST。
#pragma once

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace oj {

class HttpClient {
public:
    // 全局初始化 Winsock（首次调用时）
    static bool ensureWsa() {
        static bool inited = false;
        if (inited) return true;
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        inited = true;
        return true;
    }

    // 简单 URL 解析：http://host:port，返回 host/port，默认 80
    static bool parseUrl(const std::string& url, std::string& host, int& port) {
        std::string u = url;
        if (u.rfind("http://", 0) == 0) u = u.substr(7);
        else if (u.rfind("https://", 0) == 0) return false; // 暂不支持 https（生产需加 TLS）
        size_t slash = u.find('/');
        std::string authority = (slash == std::string::npos) ? u : u.substr(0, slash);
        port = 80;
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos && colon != authority.size() - 1) {
            host = authority.substr(0, colon);
            port = atoi(authority.substr(colon + 1).c_str());
        } else {
            host = authority;
        }
        return !host.empty();
    }

    // 同步 HTTP 请求：返回响应体字符串；失败返回空串
    // 注意：本实现为同步阻塞，供客户端低并发场景使用。
    static std::string request(const std::string& method, const std::string& url,
                               const std::string& body, const std::string& token) {
        if (!ensureWsa()) return "";
        std::string host; int port;
        if (!parseUrl(url, host, port)) return "";

        // 提取 path
        std::string u = url;
        if (u.rfind("http://", 0) == 0) u = u.substr(7);
        std::string path = "/";
        size_t slash = u.find('/');
        if (slash != std::string::npos) path = u.substr(slash);
        else {
            // 无 path，可能需要带 query（从 ? 开始）
            size_t qm = u.find('?');
            if (qm != std::string::npos) path = "/" + u.substr(qm);
            else path = "/";
        }

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return "";

        // 超时设置
        DWORD timeout = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(s); return "";
        }

        // 构造请求头
        std::string req = method + " " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "Connection: close\r\n";
        req += "User-Agent: ojcore/1.0\r\n";
        req += "Accept: application/json\r\n";
        if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
        if (!body.empty()) {
            req += "Content-Type: application/json\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "\r\n";
        req += body;

        // 发送
        int sent = 0;
        while (sent < (int)req.size()) {
            int n = send(s, req.data() + sent, (int)req.size() - sent, 0);
            if (n <= 0) { closesocket(s); return ""; }
            sent += n;
        }

        // 接收
        std::string resp;
        char buf[4096];
        int n;
        while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
            resp.append(buf, n);
            if (resp.size() > 16 * 1024 * 1024) break; // 16MB 上限
        }
        closesocket(s);

        // 分离 header/body
        size_t hdrEnd = resp.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) return "";
        std::string bodyOut = resp.substr(hdrEnd + 4);

        // 处理 chunked 编码（简单场景）
        // 中间层返回 Content-Length，此处不处理 chunked
        return bodyOut;
    }

    static std::string get(const std::string& url, const std::string& token = "") {
        return request("GET", url, "", token);
    }
    static std::string post(const std::string& url, const std::string& jsonBody, const std::string& token = "") {
        return request("POST", url, jsonBody, token);
    }
};

} // namespace oj

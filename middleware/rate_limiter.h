// rate_limiter.h —— 滑动窗口限流器（按 client 维度）
#pragma once

#include <map>
#include <deque>
#include <chrono>
#include <string>

namespace mw {

class RateLimiter {
public:
    RateLimiter(int max_count = 60, int window_seconds = 60)
        : max_count_(max_count), window_(window_seconds) {}

    bool allow(const std::string& client) {
        auto now = std::chrono::steady_clock::now();
        auto& q = hits_[client];
        double win = (double)window_;
        while (!q.empty() && std::chrono::duration<double>(now - q.front()).count() >= win)
            q.pop_front();
        if ((int)q.size() >= max_count_) return false;
        q.push_back(now);
        return true;
    }

private:
    int max_count_;
    int window_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> hits_;
};

} // namespace mw

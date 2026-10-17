#pragma once
#include <functional>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>

// 线程管理壳：固定大小线程池 + bounded task queue + graceful shutdown
class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads, size_t max_queue_size = 64);
    ~ThreadPool();

    // 提交任务，返回 future 用于等待结果
    std::future<void> submit(std::function<void()> task);

    // 获取当前队列中待处理任务数
    size_t pending_count() const;

    // 获取线程池大小
    size_t pool_size() const { return workers_.size(); }

    // 优雅关闭：等待所有已提交任务完成
    void shutdown();

    // 非阻塞检查是否已满
    bool is_full() const;

private:
    std::vector<std::thread> workers_;
    std::queue<std::packaged_task<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable full_cv_;  // 背压：队列满时阻塞生产者
    std::atomic<bool> stop_{false};
    size_t max_queue_size_;
};

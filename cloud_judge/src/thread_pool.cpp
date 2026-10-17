#include "thread_pool.h"

ThreadPool::ThreadPool(size_t num_threads, size_t max_queue_size)
    : max_queue_size_(max_queue_size) {
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this] {
            while (true) {
                std::packaged_task<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stop_.load() || !tasks_.empty(); });
                    if (stop_.load() && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                // 通知生产者队列已有空位
                full_cv_.notify_one();
                // 执行任务（异常被 packaged_task 内部捕获，通过 future 传播）
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

std::future<void> ThreadPool::submit(std::function<void()> task) {
    std::packaged_task<void()> pt(std::move(task));
    auto fut = pt.get_future();
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // 背压：队列满时阻塞生产者，直到有空位或线程池停止
        full_cv_.wait(lock, [this] { return stop_.load() || tasks_.size() < max_queue_size_; });
        if (stop_.load()) {
            throw std::runtime_error("ThreadPool: submit after shutdown");
        }
        tasks_.push(std::move(pt));
    }
    cv_.notify_one();
    return fut;
}

size_t ThreadPool::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

bool ThreadPool::is_full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size() >= max_queue_size_;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_.load()) return;
        stop_.store(true);
    }
    cv_.notify_all();
    full_cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

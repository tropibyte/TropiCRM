// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/thread_pool.hpp"

#include <exception>

#include "util/log.hpp"

namespace tcrm {

ThreadPool::ThreadPool(size_t threads, std::string name, size_t max_queue)
    : name_(std::move(name)), max_queue_(max_queue) {
    if (threads == 0) threads = 1;
    threads_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        threads_.emplace_back([this] { run(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) return false;
        if (max_queue_ != 0 && queue_.size() >= max_queue_) return false;
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    stopping_ = true;
    cv_.notify_all();
    for (std::thread& t : threads_) {
        if (t.joinable()) t.join();
    }
}

size_t ThreadPool::queued() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
}

size_t ThreadPool::active() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_;
}

void ThreadPool::run() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        try {
            task();
        } catch (const std::exception& e) {
            log::error(name_, "task failed: ", e.what());
        } catch (...) {
            log::error(name_, "task failed with an unknown exception");
        }
        std::lock_guard<std::mutex> lock(mu_);
        --active_;
    }
}

}  // namespace tcrm

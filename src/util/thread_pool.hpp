// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tcrm {

/// A fixed-size pool of worker threads draining a FIFO queue.
class ThreadPool {
public:
    /// `max_queue` of 0 means unbounded.
    ThreadPool(size_t threads, std::string name, size_t max_queue = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// Queues a task. Returns false when the pool is shutting down or the
    /// queue is full.
    bool submit(std::function<void()> task);

    /// Stops accepting work, lets queued tasks finish and joins the workers.
    void shutdown();

    size_t thread_count() const { return threads_.size(); }
    size_t queued() const;
    size_t active() const;

private:
    void run();

    std::string name_;
    size_t max_queue_;
    std::vector<std::thread> threads_;
    std::deque<std::function<void()>> queue_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    size_t active_ = 0;
};

}  // namespace tcrm

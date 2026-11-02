#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

class BackgroundWork {
public:
    explicit BackgroundWork(size_t workers, std::function<void(std::exception_ptr)> onError = {})
        : m_onError(std::move(onError)) {
        try {
            for (size_t i = 0; i < workers; ++i) m_threads.emplace_back([this] { run(); });
        } catch (...) {
            stop();
            throw;
        }
    }
    ~BackgroundWork() { stop(); }
    BackgroundWork(const BackgroundWork&) = delete;
    BackgroundWork& operator=(const BackgroundWork&) = delete;

    void post(uintptr_t key, std::function<void()> action) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return;
        if (key) for (auto& pending : m_queue) {
            if (pending.key == key) {
                pending.action = std::move(action);
                return;
            }
        }
        m_queue.push_back({key, std::move(action)});
        m_ready.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            m_queue.clear();
        }
        m_ready.notify_all();
        for (auto& thread : m_threads) if (thread.joinable()) thread.join();
    }

private:
    struct Work { uintptr_t key; std::function<void()> action; };
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<Work> m_queue;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
    std::function<void(std::exception_ptr)> m_onError;

    void run() {
        for (;;) {
            Work work;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_ready.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                if (m_stopping) return;
                work = std::move(m_queue.front());
                m_queue.pop_front();
            }
            try { work.action(); }
            catch (...) {
                if (m_onError) {
                    try { m_onError(std::current_exception()); }
                    catch (...) {}
                }
            }
        }
    }
};

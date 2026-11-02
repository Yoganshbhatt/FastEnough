#pragma once

#include <memory>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

template<class Task>
void runDeviceTasks(size_t count, Task&& task) {
    if (count == 0) return;
    if (count == 1) { task(0); return; }
    std::atomic<size_t> next{0};
    std::mutex errorMutex;
    std::exception_ptr error;
    auto work = [&]() {
        for (;;) {
            const size_t index = next.fetch_add(1);
            if (index >= count) return;
            try { task(index); }
            catch (...) {
                std::lock_guard<std::mutex> lock(errorMutex);
                if (!error) error = std::current_exception();
            }
        }
    };
    std::vector<std::jthread> workers;
    workers.reserve(std::min<size_t>(count, 4) - 1);
    for (size_t i = 1; i < std::min<size_t>(count, 4); ++i) {
        try { workers.emplace_back(work); }
        catch (const std::system_error&) { break; }
    }
    work();
    workers.clear();
    if (error) std::rethrow_exception(error);
}

// Session objects and indices remain valid until the store is destroyed.
template<class T>
class DeviceSessionStore {
public:
    explicit DeviceSessionStore(int reserved = 2) {
        for (int i = 0; i < reserved; ++i) m_records.push_back({{}, std::make_unique<T>()});
    }

    int ensure(const std::string& identity) {
        if (identity.empty()) throw std::invalid_argument("Empty device identity");
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_slots.find(identity);
        if (found != m_slots.end()) return found->second;
        std::string storedIdentity(identity);
        for (int i = 0; i < (int)m_records.size(); ++i) {
            if (!m_records[i].identity.empty()) continue;
            m_slots.emplace(identity, i);
            m_records[i].identity.swap(storedIdentity);
            return i;
        }
        const int slot = (int)m_records.size();
        m_records.push_back({std::move(storedIdentity), std::make_unique<T>()});
        try { m_slots.emplace(identity, slot); }
        catch (...) { m_records.pop_back(); throw; }
        return slot;
    }

    int find(const std::string& identity) const {
        if (identity.empty()) return -1;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_slots.find(identity);
        return found == m_slots.end() ? -1 : found->second;
    }

    T& at(int slot) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return *m_records.at(slot).value;
    }

    int size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return (int)m_records.size();
    }

    std::vector<std::string> identities() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> result;
        result.reserve(m_records.size());
        for (const auto& record : m_records) result.push_back(record.identity);
        return result;
    }

private:
    struct Record { std::string identity; std::unique_ptr<T> value; };
    mutable std::mutex m_mutex;
    std::vector<Record> m_records;
    std::unordered_map<std::string, int> m_slots;
};

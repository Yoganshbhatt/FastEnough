#pragma once
#include <atomic>
#include <memory>
#include <mutex>

template<class Client>
class TransferCancellation {
public:
    explicit TransferCancellation(std::atomic<bool>& requested) : m_requested(requested) {}

    class Registration {
    public:
        Registration(TransferCancellation& owner, Client& client) : m_owner(owner), m_client(client) {
            std::lock_guard<std::mutex> lock(m_owner.m_mutex);
            m_next = m_owner.m_first;
            if (m_next) m_next->m_previous = this;
            m_owner.m_first = this;
            if (m_owner.m_requested) m_client.interruptBlocking();
        }
        ~Registration() {
            std::lock_guard<std::mutex> lock(m_owner.m_mutex);
            if (m_previous) m_previous->m_next = m_next;
            else m_owner.m_first = m_next;
            if (m_next) m_next->m_previous = m_previous;
        }
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;

    private:
        friend class TransferCancellation;
        TransferCancellation& m_owner;
        Client& m_client;
        Registration* m_previous = nullptr;
        Registration* m_next = nullptr;
    };

    std::unique_ptr<Registration> track(Client& client) {
        return std::make_unique<Registration>(*this, client);
    }

    void requestStop() {
        m_requested = true;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto* registration = m_first; registration; registration = registration->m_next)
            registration->m_client.interruptBlocking();
    }

private:
    std::atomic<bool>& m_requested;
    std::mutex m_mutex;
    Registration* m_first = nullptr;
};

#pragma once
#include <chrono>
#include <cstdint>
#include <unordered_map>

enum class BatchState { Preparing, Queued, Running, Paused, Verifying, Completed, Failed, Stopped, WaitingConflict };

class TransferResultRetention {
public:
    using Clock = std::chrono::steady_clock;

    bool shouldRemove(uint64_t id, BatchState state, Clock::time_point now,
        bool inspecting, bool needsInteraction, bool hasErrors) {
        const bool finished = state == BatchState::Completed || state == BatchState::Failed || state == BatchState::Stopped;
        if (!finished || needsInteraction) {
            m_finishedSince.erase(id);
            return false;
        }
        const auto entry = m_finishedSince.try_emplace(id, now).first;
        const auto lifetime = std::chrono::seconds(state == BatchState::Failed || hasErrors ? 15 :
            state == BatchState::Stopped ? 3 : 5);
        if (inspecting || now - entry->second < lifetime) return false;
        m_finishedSince.erase(entry);
        return true;
    }

private:
    std::unordered_map<uint64_t, Clock::time_point> m_finishedSince;
};

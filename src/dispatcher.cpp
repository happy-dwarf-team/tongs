#include "dispatcher.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace tongs {

Dispatcher::Dispatcher(std::function<std::uint64_t()> now_ms) : now_ms_(std::move(now_ms)) {}

void Dispatcher::Withdraw(Entry& entry, const char* error) {
    entry.state = State::Withdrawn;
    entry.outcome.error = error;
    entry.outcome.queued_ms = now_ms_() - entry.queued_at;
}

Dispatcher::Outcome Dispatcher::Run(Kind kind, Job job, int timeout_ms) {
    auto entry = std::make_shared<Entry>();
    entry->kind = kind;
    entry->job = std::move(job);
    entry->queued_at = now_ms_();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    std::unique_lock lock(mutex_);
    queue_.push_back(entry);
    for (;;) {
        if (entry->state == State::Done || entry->state == State::Withdrawn) break;
        if (entry->state == State::Running) {
            changed_.wait(lock);  // a started job always finishes
            continue;
        }
        if (changed_.wait_until(lock, deadline) == std::cv_status::timeout && entry->state == State::Queued) {
            queue_.erase(std::find(queue_.begin(), queue_.end(), entry));
            Withdraw(*entry, "main thread busy");
        }
    }
    return entry->outcome;
}

void Dispatcher::RunBatch(std::uint64_t frame) {
    std::unique_lock lock(mutex_);
    for (std::size_t left = queue_.size(); left > 0 && !queue_.empty(); --left) {
        const std::shared_ptr<Entry> entry = queue_.front();
        queue_.pop_front();
        entry->state = State::Running;
        const std::uint64_t started = now_ms_();
        entry->outcome.queued_ms = started - entry->queued_at;
        lock.unlock();
        std::string answer;
        bool ok = true;
        try {
            answer = entry->job();
        } catch (...) {  // nothing may unwind into the game's frame function
            ok = false;
        }
        const std::uint64_t finished = now_ms_();
        lock.lock();
        entry->outcome.ran = ok;
        if (!ok) entry->outcome.error = "the job failed";
        entry->outcome.answer = std::move(answer);
        entry->outcome.run_ms = finished - started;
        entry->outcome.frame = frame;
        entry->state = State::Done;
        changed_.notify_all();
        if (entry->kind == Kind::GameCall) break;
    }
}

int Dispatcher::CancelQueued() {
    std::lock_guard lock(mutex_);
    const int count = static_cast<int>(queue_.size());
    for (const auto& entry : queue_) Withdraw(*entry, "cancelled");
    queue_.clear();
    changed_.notify_all();
    return count;
}

}  // namespace tongs

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace tongs {

// Runs jobs on the game's main thread, after an outer frame (docs/design.md,
// "Dispatcher"). A pipe thread queues a job with Run() and waits for it; the
// frame hook calls RunBatch() on the main thread.
//
// - A job not started before its limit is withdrawn and never runs.
// - CancelQueued() withdraws every job that has not started.
// - A started job always finishes; Run() waits for it even past the limit.
// - One FIFO for all clients. A batch runs the jobs queued when it starts;
//   a game call ends the batch, so the next job sees its effect a frame later.
class Dispatcher {
public:
    enum class Kind { Read, GameCall };

    struct Outcome {
        bool ran = false;
        std::string error;  // when !ran: "main thread busy" or "cancelled"
        std::string answer;  // what the job returned
        std::uint64_t queued_ms = 0;  // from Run() to the start of the job (or to the withdrawal)
        std::uint64_t run_ms = 0;
        std::uint64_t frame = 0;  // the frame counter passed to the batch that ran the job
    };

    using Job = std::function<std::string()>;

    explicit Dispatcher(std::function<std::uint64_t()> now_ms);

    Outcome Run(Kind kind, Job job, int timeout_ms);
    void RunBatch(std::uint64_t frame);
    // Withdraws the jobs that have not started; returns how many.
    int CancelQueued();

private:
    enum class State { Queued, Running, Done, Withdrawn };

    struct Entry {
        Kind kind = Kind::Read;
        Job job;
        State state = State::Queued;
        std::uint64_t queued_at = 0;
        Outcome outcome;
    };

    void Withdraw(Entry& entry, const char* error);

    std::function<std::uint64_t()> now_ms_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<Entry>> queue_;
};

}  // namespace tongs

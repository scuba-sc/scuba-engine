// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// A fixed pool of worker threads with three priority lanes. Jobs never run on the submitting
// thread. A job that throws is caught and logged; it never takes a worker down. The destructor
// drops jobs that have not started and joins the workers after the running ones finish.
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace scuba {

enum class JobPriority : uint8_t { High = 0, Normal = 1, Low = 2 };

class JobSystem {
public:
    // workers == 0: hardware threads minus one (main thread), at least one.
    explicit JobSystem(unsigned workers = 0);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void Submit(std::function<void()> job, JobPriority priority = JobPriority::Normal);
    // Blocks until no job is queued or running.
    void WaitIdle();

    unsigned workers() const noexcept { return static_cast<unsigned>(threads_.size()); }
    size_t queued() const;

private:
    void Run();

    static constexpr size_t kLanes = 3;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<std::function<void()>> lanes_[kLanes];
    size_t running_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace scuba

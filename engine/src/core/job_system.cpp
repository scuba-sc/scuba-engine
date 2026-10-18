// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/core/job_system.h"
#include "scuba/core/log.h"

#include <algorithm>
#include <exception>
#include <string>

namespace scuba {

JobSystem::JobSystem(unsigned workers) {
    if (workers == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        workers = hw > 1 ? hw - 1 : 1;
    }
    threads_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) threads_.emplace_back([this] { Run(); });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard<std::mutex> hold(mutex_);
        stopping_ = true;
        for (auto& lane : lanes_) lane.clear();
    }
    wake_.notify_all();
    for (auto& t : threads_) t.join();
}

void JobSystem::Submit(std::function<void()> job, JobPriority priority) {
    if (!job) return;
    {
        std::lock_guard<std::mutex> hold(mutex_);
        if (stopping_) return;
        lanes_[static_cast<size_t>(priority)].push_back(std::move(job));
    }
    wake_.notify_one();
}

void JobSystem::WaitIdle() {
    std::unique_lock<std::mutex> hold(mutex_);
    idle_.wait(hold, [this] {
        return running_ == 0 &&
               std::all_of(std::begin(lanes_), std::end(lanes_), [](const auto& l) { return l.empty(); });
    });
}

size_t JobSystem::queued() const {
    std::lock_guard<std::mutex> hold(mutex_);
    size_t n = 0;
    for (const auto& lane : lanes_) n += lane.size();
    return n;
}

void JobSystem::Run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> hold(mutex_);
            wake_.wait(hold, [this] {
                return stopping_ || std::any_of(std::begin(lanes_), std::end(lanes_),
                                                [](const auto& l) { return !l.empty(); });
            });
            if (stopping_) return;
            for (auto& lane : lanes_) {
                if (lane.empty()) continue;
                job = std::move(lane.front());
                lane.pop_front();
                break;
            }
            ++running_;
        }
        try {
            job();
        } catch (const std::exception& e) {
            log::Error("jobs", std::string("job threw: ") + e.what());
        } catch (...) {
            log::Error("jobs", "job threw a non-standard exception");
        }
        {
            std::lock_guard<std::mutex> hold(mutex_);
            --running_;
        }
        idle_.notify_all();
    }
}

}  // namespace scuba

// SPDX-License-Identifier: GPL-3.0-only
#include "scuba/core/log.h"

#include <atomic>
#include <deque>
#include <mutex>

namespace scuba::log {
namespace {

constexpr size_t kRing = 2048;

struct State {
    std::mutex mutex;
    std::deque<Line> ring;
    uint64_t seq = 0;
    std::atomic<Sink> sink{nullptr};
};

State& S() {
    static State state;
    return state;
}

}  // namespace

void SetSink(Sink sink) { S().sink.store(sink, std::memory_order_release); }

void Write(Level level, std::string_view channel, std::string_view text) {
    Line line;
    line.level = level;
    line.channel.assign(channel);
    line.text.assign(text);
    {
        std::lock_guard<std::mutex> hold(S().mutex);
        line.seq = ++S().seq;
        S().ring.push_back(line);
        if (S().ring.size() > kRing) S().ring.pop_front();
    }
    if (Sink sink = S().sink.load(std::memory_order_acquire)) sink(line);
}

std::vector<Line> Since(uint64_t after) {
    std::lock_guard<std::mutex> hold(S().mutex);
    std::vector<Line> out;
    for (const auto& line : S().ring)
        if (line.seq > after) out.push_back(line);
    return out;
}

const char* LevelName(Level level) {
    switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    }
    return "?";
}

}  // namespace scuba::log

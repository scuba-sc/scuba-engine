// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Engine log: one process-wide sink plus a bounded ring the UI console reads. Any thread.
// sco-core kernel lines are forwarded here by ScoHost with the "sco" channel.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scuba::log {

enum class Level : uint8_t { Debug, Info, Warn, Error };

struct Line {
    uint64_t seq = 0;   // increasing; the console uses it to find new lines
    Level level = Level::Info;
    std::string channel;
    std::string text;
};

using Sink = void (*)(const Line& line);

void SetSink(Sink sink);   // in addition to the ring; nullptr removes it
void Write(Level level, std::string_view channel, std::string_view text);
// Lines with seq > after, oldest first (at most the ring's capacity).
std::vector<Line> Since(uint64_t after);
const char* LevelName(Level level);

inline void Info(std::string_view channel, std::string_view text) { Write(Level::Info, channel, text); }
inline void Warn(std::string_view channel, std::string_view text) { Write(Level::Warn, channel, text); }
inline void Error(std::string_view channel, std::string_view text) { Write(Level::Error, channel, text); }

}  // namespace scuba::log

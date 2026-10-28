#pragma once
#include "sc_audio.h"
#include <set>
#include <string>

namespace scuba::hostd::mock_game {
struct ServiceDef;
// Synthetic registry only; these names do not represent game assets.
std::set<std::string>& AudioNames();
const ServiceDef& AudioService();
}

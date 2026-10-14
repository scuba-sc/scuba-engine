#pragma once
// The synthetic console variables game.cvars reads and game.cvars_write changes (cvars.cpp).
#include <cstdint>
#include <map>
#include <string>

namespace scuba::hostd::mock_game {

std::map<std::string, uint32_t>& CvarValues();

}  // namespace scuba::hostd::mock_game

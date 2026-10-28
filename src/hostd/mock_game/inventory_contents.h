#pragma once
// Host-only synthetic container data; identifiers and volumes are invented.
#include "sc_container_contents.h"
#include <map>
#include <vector>

namespace scuba::hostd::mock_game {
struct ServiceDef;
struct InventoryContentsValues {
    std::map<uint64_t, std::vector<sc_container_item>> containers;
    uint64_t generation = 1;
    bool available = true;
    bool volumeKnown = true;
};
InventoryContentsValues& InventoryContents();
const ServiceDef& InventoryContentsService();
}

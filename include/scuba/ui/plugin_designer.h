#pragma once
// SPDX-License-Identifier: GPL-3.0-only
#include <array>
#include <future>
#include <string>
#include <vector>

namespace scuba::ui {
class PluginDesigner {
public:
    void Draw(bool* open);
private:
    struct Node {
        int kind = 0;
        std::array<char, 256> label{};
        std::array<char, 256> text{};
        bool disabled = false;
        float value = 0.5f, minimum = 0, maximum = 1;
    };
    std::array<char, 32> id_{'d','e','s','i','g','n','e','d','_','p','a','n','e','l'};
    std::array<char, 64> title_{'D','e','s','i','g','n','e','d',' ','p','a','n','e','l'};
    std::vector<Node> nodes_;
    int selected_ = -1, addKind_ = 1;
    bool placed_ = false;
    std::string status_;
    std::future<std::string> exporting_;
};
}

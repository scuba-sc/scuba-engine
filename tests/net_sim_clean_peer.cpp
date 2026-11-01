// SPDX-License-Identifier: GPL-3.0-only
#include <iostream>
#include <string>

int main() {
    std::string line;
    while (std::getline(std::cin, line))
        std::cout << R"({"port":41000})" << '\n' << std::flush;
    return 0;
}

#include <iostream>
#include <string>

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::cout << R"({"port":41000})" << '\n' << std::flush;
    }
    // Process::Stop must distinguish confirmed termination from clean retirement.
    return 17;
}

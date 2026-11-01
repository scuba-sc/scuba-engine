// Opt-in experimental, stdio-only host of the reserved typed mock table.
#include "hostd/reserved_mock_host.h"
#include <chrono>
#include <iostream>
#include <string>

int main() {
    auto& host = scuba::hostd::ReservedMockHost::Instance();
    if (!host.Start()) { std::cerr << "reserved mock startup failed\n"; return 1; }
    // Bound storage even for a peer that never sends a newline.
    constexpr std::size_t maxLine = 65536;
    std::string line;
    line.reserve(maxLine);
    bool oversized = false;
    char c;
    auto dispatch = [&] {
        if (oversized) std::cout << R"({"id":null,"error":{"code":-32600,"message":"request exceeds 65536 bytes"}})";
        else std::cout << host.registry().dispatch_text(line, {});
        std::cout << '\n' << std::flush;
        line.clear(); oversized = false;
    };
    while (std::cin.get(c)) {
        if (c == '\n') { dispatch(); continue; }
        if (line.size() < maxLine) line.push_back(c);
        else oversized = true;
    }
    if (!line.empty() || oversized) dispatch();
    if (host.Close() != sco::Result::Ok ||
        host.RetireUntil(std::chrono::steady_clock::now() + std::chrono::seconds(5)) !=
            sco::reserved::RetireResult::QuiescedRetained) {
        std::cerr << "reserved mock retirement failed; retained until process exit\n";
        return 1;
    }
    return 0;
}

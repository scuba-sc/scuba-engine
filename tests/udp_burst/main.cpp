// Diagnostic regression: queue the complete burst before reading any datagram.
// Uses only the selected host-source SDK's socket interface; no game or service.
#include <sco/net/udp.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>

template<class Socket> void ReportBuffer(const Socket& socket) {
    if constexpr (requires { socket.RecvBufBytes(); })
        std::printf("sdk_receive_buffer_bytes=%u\n", socket.RecvBufBytes());
    else
        std::puts("sdk_receive_buffer_bytes=unavailable_in_selected_sdk");
}

int main() {
    constexpr uint32_t count = 384;
    constexpr size_t packetBytes = 64;
    std::string why;
    auto receiver = sco::net::udp::Socket::Open(0, true, &why);
    auto sender = sco::net::udp::Socket::Open(0, true, &why);
    if (!receiver || !sender) { std::fprintf(stderr, "socket open: %s\n", why.c_str()); return 1; }
    ReportBuffer(*receiver);
    sco::net::Endpoint to{};
    to.family = sco::net::Endpoint::kIpv4;
    to.port = receiver->Port(); to.addr[0] = 127; to.addr[3] = 1;
    std::array<uint8_t, packetBytes> packet{};
    for (uint32_t i = 0; i < count; ++i) {
        std::memcpy(packet.data(), &i, sizeof i);
        if (!sender->Send(to, packet.data(), packet.size())) {
            std::fprintf(stderr, "send refused at %u; no send retry\n", i); return 1;
        }
    }
    std::array<bool, count> seen{};
    uint32_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (received < count && std::chrono::steady_clock::now() < deadline) {
        sco::net::Endpoint from{};
        size_t length = 0;
        if (!receiver->Receive(&from, packet.data(), packet.size(), &length)) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining > 0) receiver->Wait(static_cast<uint32_t>(remaining));
            continue;
        }
        uint32_t index = count;
        if (length == packetBytes) std::memcpy(&index, packet.data(), sizeof index);
        if (index >= count || seen[index] || from.port != sender->Port()) {
            std::fputs("malformed, duplicate or unexpected datagram\n", stderr); return 1;
        }
        seen[index] = true; ++received;
    }
    std::printf("sent=%u received=%u lost=%u payload_bytes=%zu\n", count, received, count - received, packetBytes);
    return received == count ? 0 : 1;
}

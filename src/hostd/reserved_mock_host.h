#pragma once

#include "hostd/handlers.h"
#include <sco/reserved_session.h>

#include <chrono>

namespace scuba::hostd {

// Experimental host-source composition. Instance and all objects captured by its
// Registry are retained for the process lifetime, including after either terminal
// verdict. This is not the legacy hostd or an application Tick implementation.
class ReservedMockHost {
public:
    static ReservedMockHost& Instance();
    bool Start();
    sco::Result Close() noexcept;
    sco::reserved::RetireResult RetireUntil(std::chrono::steady_clock::time_point deadline) noexcept;
    sco::reserved::Phase phase() noexcept;
    const Registry& registry() const noexcept { return registry_; }

    ReservedMockHost(const ReservedMockHost&) = delete;
    ReservedMockHost& operator=(const ReservedMockHost&) = delete;
private:
    friend struct ReservedMockHostTestAccess;
    ReservedMockHost();
    bool StartImpl(bool abandon);
    void RegisterHandlers();
    const sco::reserved::MockTable& table() const;
    Registry registry_;
    sco::reserved::CoreBinding binding_;
    sco::reserved::Session session_;
    const sco::reserved::MockTable* table_ = nullptr;
    bool attempted_ = false;
};

} // namespace scuba::hostd

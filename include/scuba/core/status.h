// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Success-or-reason, the same convention as sco::vfs::Result: ok when error is empty. Engine APIs
// return it instead of throwing across subsystem boundaries; exceptions stay inside a subsystem.
#include <string>
#include <utility>

namespace scuba {

struct Status {
    std::string error;

    bool ok() const noexcept { return error.empty(); }
    explicit operator bool() const noexcept { return ok(); }

    static Status Ok() { return {}; }
    static Status Fail(std::string why) {
        Status s;
        s.error = why.empty() ? std::string("unspecified error") : std::move(why);
        return s;
    }
};

}  // namespace scuba

#pragma once

// Defined by the test executable; referenced only in the instrumented host build.
namespace scuba::hostd::reserved_copy_probe {
bool ReleasePinEarly() noexcept;
void BeforeNameCopy();
bool ThrowAfterCopy() noexcept;
void DuringUnwind() noexcept;
}

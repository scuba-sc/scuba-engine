#pragma once
// A tiny assertion helper for the ctest executables: CHECK records a failure and continues;
// CheckExit() returns the process exit code (1 when any CHECK failed).
#include <cstdio>

inline int& CheckFailures() {
    static int failures = 0;
    return failures;
}

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++CheckFailures();                                                       \
        }                                                                            \
    } while (0)

inline int CheckExit(const char* name) {
    if (CheckFailures()) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", name, CheckFailures());
        return 1;
    }
    std::printf("%s: all checks passed\n", name);
    return 0;
}

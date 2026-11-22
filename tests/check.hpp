#pragma once

#include <cstdio>
#include <cstdlib>

// Always evaluates condition and aborts on failure, even in Release builds.
// Test setup/work is performed before calling CHECK; never call schedule/pop
// as the condition expression because NDEBUG must not remove simulation work.
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr,                                                \
                         "CHECK failed: %s at %s:%d\n",                         \
                         #condition,                                            \
                         __FILE__,                                              \
                         __LINE__);                                             \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

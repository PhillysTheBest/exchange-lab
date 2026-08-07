// tests/test_harness.hpp — a tiny hand-rolled check macro. No test framework is
// installed in this toolchain (no GoogleTest/Catch2 package manager available —
// see the plan doc's environment notes), and pulling one in as a header-only
// vendor drop would be more machinery than four small test files justify.
#pragma once

#include <cstdio>

namespace px::test {
inline int g_checks = 0;
inline int g_failures = 0;
} // namespace px::test

#define PX_CHECK(cond)                                                            \
    do {                                                                          \
        ++::px::test::g_checks;                                                   \
        if (!(cond)) {                                                            \
            ++::px::test::g_failures;                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                         \
    } while (0)

#define PX_TEST_REPORT_AND_RETURN()                                                          \
    do {                                                                                      \
        std::printf("%d/%d checks passed\n", ::px::test::g_checks - ::px::test::g_failures,   \
                     ::px::test::g_checks);                                                   \
        return ::px::test::g_failures == 0 ? 0 : 1;                                           \
    } while (0)

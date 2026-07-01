#pragma once
#include <iostream>
#include <cmath>
#include <string>

// Tiny zero-dependency test harness. Each test file defines run_tests() and
// uses the XR_CHECK* macros; main() returns non-zero if any check failed, which
// CTest reports as a failure.

namespace xrune::test {
inline int g_failures = 0;
inline int g_checks = 0;
}

#define XR_CHECK(cond)                                                          \
    do {                                                                       \
        ++xrune::test::g_checks;                                               \
        if (!(cond)) {                                                         \
            std::cerr << "CHECK FAILED: " #cond " @ " << __FILE__ << ":"       \
                      << __LINE__ << "\n";                                     \
            ++xrune::test::g_failures;                                         \
        }                                                                      \
    } while (0)

#define XR_CHECK_NEAR(a, b, eps)                                                \
    do {                                                                       \
        ++xrune::test::g_checks;                                               \
        double _xa = (a), _xb = (b), _xe = (eps);                             \
        double _xd = std::fabs(_xa - _xb);                                     \
        if (_xd > _xe) {                                                       \
            std::cerr << "CHECK_NEAR FAILED: " #a "=" << _xa << " vs " #b "="  \
                      << _xb << " (|diff|=" << _xd << " > " << _xe << ") @ "    \
                      << __FILE__ << ":" << __LINE__ << "\n";                  \
            ++xrune::test::g_failures;                                         \
        }                                                                      \
    } while (0)

#define XR_RUN(name)                                                            \
    do {                                                                       \
        std::cerr << "[ run ] " << name << "\n";                              \
    } while (0)

#define XR_MAIN_REPORT()                                                       \
    do {                                                                       \
        std::cerr << "[done] " << xrune::test::g_checks << " checks, "          \
                  << xrune::test::g_failures << " failures\n";                  \
        return xrune::test::g_failures == 0 ? 0 : 1;                            \
    } while (0)

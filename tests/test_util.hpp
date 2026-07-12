/*
 * Xrune — a real-time audio engine, graph and instancing system.
 * Copyright (C) 2026 Johann Philippe
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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

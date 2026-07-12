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
#include <atomic>

namespace xrune::rt {

// Real-time allocation guard. The audio path wraps its work in a no_alloc_scope;
// a debug build (e.g. the rt-safety test) can override global operator new/delete
// to increment `alloc_violations` whenever an allocation happens while any
// thread's `no_alloc_depth > 0`. In normal builds nothing overrides new/delete,
// so the guard is just a couple of thread-local increments (negligible) and the
// counter is never touched.
//
// `no_alloc_depth` is thread-local (each RT thread tracks its own nesting);
// `alloc_violations` is a global atomic so violations from worker threads and
// the audio thread aggregate into one count the control thread can read.
inline thread_local int no_alloc_depth = 0;
inline std::atomic<unsigned long long> alloc_violations{0};

struct no_alloc_scope {
    no_alloc_scope() { ++no_alloc_depth; }
    ~no_alloc_scope() { --no_alloc_depth; }
    no_alloc_scope(const no_alloc_scope&) = delete;
    no_alloc_scope& operator=(const no_alloc_scope&) = delete;
};

} // namespace xrune::rt

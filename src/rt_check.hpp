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

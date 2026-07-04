#pragma once
// Static Faust node: wraps a Faust-*generated* DSP class (compiled ahead of
// time from a .dsp with `faust -cn MyDsp -a <standard-arch> file.dsp`, double
// precision). No libfaust/LLVM at runtime — just the generated class and
// Faust's standard headers.
//
// Usage:
//   #include "generated_synth.hpp"   // defines class `synth : public dsp`
//   #include "node/faust/faust_static.hpp"
//   ...
//   registry.add("synth", [](const node_args&){ return std::make_unique<xrune::faust_static<synth>>(); });
//
// Generate with a standard architecture that produces a `dsp` subclass, e.g.:
//   faust -lang cpp -double -cn synth -o generated_synth.hpp synth.dsp
// (the default C++ output is a `class synth : public dsp`, which is exactly
// what this wrapper needs — no custom architecture file required).

#include "faust_common.hpp"
#include <memory>

namespace xrune {

template <class Dsp>
struct faust_static : faust_node_base {
    faust_static() { init_meta(); } // Dsp is default-constructible

    ::dsp* make_dsp() const override { return new Dsp(); }
    // destroy_dsp default (delete) is correct for a `new Dsp()`.
};

} // namespace xrune

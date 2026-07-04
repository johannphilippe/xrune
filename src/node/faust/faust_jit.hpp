#pragma once
// JIT Faust node: compiles Faust source at runtime via libfaust's LLVM backend
// (createDSPFactory... -> createDSPInstance). One factory (compiled once) is
// shared by the node; each voice creates its own dsp instance.
//
// Usage:
//   auto n = std::make_unique<xrune::faust_jit>("process = _ * hslider(\"g\",1,0,2,0.01);");
//   // or from a file:
//   auto n = std::make_unique<xrune::faust_jit>("synth.dsp", /*is_file=*/true);

#include "faust_common.hpp"
#include <faust/dsp/llvm-dsp.h>
#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>

namespace xrune {

struct faust_jit : faust_node_base {
    llvm_dsp_factory* factory = nullptr;
    std::string error;

    // dsp_code is Faust source, or a .dsp path when is_file is true.
    // Xrune samples are double, so -double is always ensured.
    explicit faust_jit(const std::string& dsp_code, bool is_file = false,
                       const std::string& options = "-double") {
        std::vector<std::string> toks = split(options);
        bool has_double = false;
        for (const auto& t : toks) if (t == "-double") has_double = true;
        if (!has_double) toks.emplace_back("-double");

        std::vector<const char*> argv;
        argv.reserve(toks.size());
        for (const auto& t : toks) argv.push_back(t.c_str());

        std::string err;
        if (is_file)
            factory = createDSPFactoryFromFile(dsp_code, static_cast<int>(argv.size()),
                                               argv.data(), "", err);
        else
            factory = createDSPFactoryFromString("xrune", dsp_code, static_cast<int>(argv.size()),
                                                 argv.data(), "", err);
        if (!factory) { error = err; throw std::runtime_error("faust_jit compile failed: " + err); }
        init_meta();
    }

    ~faust_jit() override {
        if (factory) deleteDSPFactory(factory);
    }

    ::dsp* make_dsp() const override { return factory->createDSPInstance(); }
    void destroy_dsp(::dsp* d) const override { delete d; }

private:
    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream ss(s);
        std::string t;
        while (ss >> t) out.push_back(t);
        return out;
    }
};

} // namespace xrune

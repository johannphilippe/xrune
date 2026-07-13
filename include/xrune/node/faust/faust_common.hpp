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
// Shared base for Faust-hosted nodes (JIT and static). Uses Faust's *standard*
// architecture headers: <faust/dsp/dsp.h> for the dsp base and
// <faust/gui/APIUI.h> for parameter discovery/control (names, ranges, defaults,
// and setParamValue by index). Xrune samples are double, so the Faust DSP must
// be double precision (compile with -double / FAUSTFLOAT=double).
//
// Fits the Xrune node model: the node type is stateless code that holds the
// immutable metadata (I/O counts, port descriptors from the DSP's UI); each
// voice gets its own Faust `dsp` instance, created in setup_state (control
// thread, with the sample rate) and released in destroy_state. Faust control
// parameters become Xrune control ports (read once per block via .first()).

#ifndef FAUSTFLOAT
#define FAUSTFLOAT double
#endif

#include "xrune/core.hpp"
#include <faust/dsp/dsp.h>
#include <faust/gui/APIUI.h>
#include <string>
#include <vector>
#include <algorithm>

namespace xrune {

// Per-voice state: pointers only (POD in the arena); the objects are heap-owned
// and created/destroyed on the control thread.
struct faust_instance_state {
    ::dsp* d = nullptr;
    APIUI* ui = nullptr;
    FAUSTFLOAT** ins = nullptr;
    FAUSTFLOAT** outs = nullptr;
};

// Base node: subclasses only provide dsp creation/destruction.
struct faust_node_base : node {
    size_t n_in = 0, n_out = 0;
    std::vector<std::string> port_names;      // stable storage for descriptor names
    std::vector<port_descriptor> port_descs;

    // ---- subclass hooks ----
    virtual ::dsp* make_dsp() const = 0;
    virtual void destroy_dsp(::dsp* d) const { delete d; }

    // Discover I/O counts + control ports from a throwaway instance. Call from
    // the most-derived constructor (so make_dsp() dispatches correctly).
    void init_meta() {
        ::dsp* tmp = make_dsp();
        n_in = static_cast<size_t>(tmp->getNumInputs());
        n_out = static_cast<size_t>(tmp->getNumOutputs());

        APIUI ui;
        tmp->buildUserInterface(&ui);
        const int np = ui.getParamsCount();
        port_names.clear(); port_names.reserve(static_cast<size_t>(np));
        for (int i = 0; i < np; ++i) {
            std::string addr = ui.getParamAddress(i);          // e.g. "/synth/freq"
            auto pos = addr.find_last_of('/');
            port_names.push_back(pos == std::string::npos ? addr : addr.substr(pos + 1));
        }
        port_descs.clear(); port_descs.reserve(static_cast<size_t>(np));
        for (int i = 0; i < np; ++i) {
            port_descriptor pd;
            pd.name = port_names[static_cast<size_t>(i)].c_str();
            pd.default_value = ui.getParamInit(i);
            pd.min_value = ui.getParamMin(i);
            pd.max_value = ui.getParamMax(i);
            port_descs.push_back(pd);
        }
        destroy_dsp(tmp);
    }

    // Override a control port's default (from DSL args), clamped to its range.
    void set_port_default(const std::string& name, sample_t v) {
        for (size_t i = 0; i < port_names.size(); ++i)
            if (port_names[i] == name) {
                port_descs[i].default_value = std::clamp(v, port_descs[i].min_value, port_descs[i].max_value);
                return;
            }
    }
    void set_port_default_index(size_t i, sample_t v) {
        if (i < port_descs.size())
            port_descs[i].default_value = std::clamp(v, port_descs[i].min_value, port_descs[i].max_value);
    }

    // ---- node interface ----
    size_t inputs_count() const override { return n_in; }
    size_t outputs_count() const override { return n_out; }
    size_t params_count() const override { return port_descs.size(); }
    const port_descriptor* params() const override { return port_descs.data(); }
    sample_t param_default(size_t i) const override { return port_descs[i].default_value; }

    size_t state_size() const override { return sizeof(faust_instance_state); }
    size_t state_align() const override { return alignof(faust_instance_state); }

    void init_state(void* s) const override {
        *static_cast<faust_instance_state*>(s) = faust_instance_state{}; // null pointers
    }

    void setup_state(void* s, size_t sample_rate, size_t /*block_size*/) const override {
        auto* st = static_cast<faust_instance_state*>(s);
        st->d = make_dsp();
        st->d->init(static_cast<int>(sample_rate));
        st->ui = new APIUI();
        st->d->buildUserInterface(st->ui);
        st->ins = n_in ? new FAUSTFLOAT*[n_in] : nullptr;
        st->outs = n_out ? new FAUSTFLOAT*[n_out] : nullptr;
    }

    void destroy_state(void* s) const override {
        auto* st = static_cast<faust_instance_state*>(s);
        if (st->d) { destroy_dsp(st->d); st->d = nullptr; }
        delete st->ui;   st->ui = nullptr;
        delete[] st->ins;  st->ins = nullptr;
        delete[] st->outs; st->outs = nullptr;
    }

    void process(void* s, const node_processing_context& ctx) const override {
        auto* st = static_cast<faust_instance_state*>(s);
        if (!st->d) { // not set up (misuse): output silence
            for (size_t c = 0; c < n_out; ++c)
                for (size_t i = 0; i < ctx.block_size; ++i) ctx.outputs[c][i] = 0.0;
            return;
        }
        // Control ports -> Faust zones (control-rate: one value per block).
        for (size_t i = 0; i < port_descs.size(); ++i)
            st->ui->setParamValue(static_cast<int>(i), ctx.params[i].first());
        // Route buffers (FAUSTFLOAT == sample_t == double).
        for (size_t c = 0; c < n_in; ++c)  st->ins[c]  = ctx.inputs[c].data;
        for (size_t c = 0; c < n_out; ++c) st->outs[c] = ctx.outputs[c].data;
        st->d->compute(static_cast<int>(ctx.block_size), st->ins, st->outs);
    }
};

} // namespace xrune

# ⬢ X̸rune

  ᚷ A real‑time audio engine for Idƴl programs. \
  ᚷ Gentle enough to be an execution engine for other programs too.

**Xrune is two things: a real-time audio engine, and the small language that
drives it.** You describe an audio graph once — in the Xrune language, or through
the C++ API — then spawn it hundreds of times as independent, cheap *voices*,
each with its own state, its own parameters, and its own place in a routing
network. The audio thread never allocates, never locks, never blocks.

Programs are `.rune` files, built from **runes** (graphs) and **sigils**
(reusable fragments).

```rune
rune drone(base = 110)
  low  = detuned(base, 0.006) :> gain(0.12)
  high = detuned(base * 2, 0.01) :> gain(0.06)
  out (low , high) :> m2s
end
```

## Components

 ᚷ **Engine** : realtime or offline DSP processing \
 ᚷ **Graph** : the rune system — combine & connect audio nodes \
 ᚷ **Nodes** : standard blocks for DSP processing \
 ᚷ **Language** : a tiny DSL to describe rune audio graphs

---

## Contents

- [Build](#build)
- [The mechanism](#the-mechanism) — how the engine actually works
- [Using the C++ API](#using-the-c-api)
- [The Xrune language](#the-xrune-language)
- [Node vocabulary](#node-vocabulary)
- [Serialization & graph export](#serialization--graph-export)
- [Faust nodes](#faust-nodes)
- [Status](#status)
- [License](#license)

---

## Build

RtAudio and readerwriterqueue are fetched by CMake. Needs C++20.

```bash
cmake -S . -B build
cmake --build build -j
cd build && ctest              # 19 suites
```

Optional Faust support:

```bash
cmake -S . -B build -DXRUNE_WITH_FAUST=ON       # static: faust-generated C++
cmake -S . -B build -DXRUNE_WITH_FAUST_LLVM=ON  # JIT: compile .dsp at runtime
```

### The `xrune` command

One binary does everything:

```bash
./build/xrune examples/drone.rune            # play it
./build/xrune -e examples/drone.rune         # check it compiles (no audio)
./build/xrune -l examples/drone.rune         # list the runes in the file
./build/xrune -j examples/drone.rune         # emit JSON (reloadable)
./build/xrune -d examples/drone.rune         # emit Graphviz DOT
./build/xrune -p examples/drone.rune -o g.png  # render a PNG graph
```

`-e/--eval` exits non-zero with `file:line:col` diagnostics, so it drops straight
into a Makefile or a CI job. Every mode except `run` uses the offline backend, so
they work headless — no audio device required.

PNG output shells out to Graphviz's **`dot`** (`apt install graphviz`). That's a
deliberate choice: linking `libgvc` would drag a heavy build dependency into an
otherwise self-contained project to do something the `dot` binary already does.
If Graphviz is missing, `xrune` says so and points you at `-d`.

`xrune --help` lists the rest (`-r/--rune`, `-o/--output`, `-s/--seconds`,
`--sample-rate`, `--block-size`, `--compact`).

### Installing / using Xrune from another project

```bash
cmake --install build --prefix /usr/local     # installs the `xrune` binary too
```

libxrune is **static by default**, so the installed `xrune` binary is
self-contained. Build a shared library with `-DXRUNE_SHARED=ON`; the installed
binaries then carry a relative rpath (`$ORIGIN/../lib`), so they find
`libxrune.so` from any prefix without `ldconfig` or `LD_LIBRARY_PATH`.

Headers install under `<prefix>/include/xrune/`, so downstream code writes:

```cpp
#include <xrune/api.hpp>
#include <xrune/node/standard_nodes.hpp>
```

and CMake finds it as a package:

```cmake
find_package(xrune REQUIRED)
target_link_libraries(my_app PRIVATE xrune::xrune)
```

### Source layout

```
include/xrune/     public headers — this is what gets installed
  core.hpp           the node interface (derive from this to write a node)
  api.hpp            the control API
  blueprint.hpp  schedule.hpp  instance.hpp  engine.hpp  serialize.hpp
  audio/             backend interface, RtAudio, offline
  util/              arena, json, mpmc_queue, worker_pool, rt_check
  node/              standard nodes, fft, multirate, faust/
  lang/              the language front-end (lexer, parser, lowering)
src/               implementation (.cpp)
apps/              xrune (the CLI), demo
tests/             the suite
```

Most headers are declarations only; the implementation is compiled into the
library. A few stay header-only **on purpose**: `core.hpp` (the interface node
authors derive from), `blueprint.hpp` (templates), and `util/arena.hpp` /
`util/mpmc_queue.hpp` / `util/rt_check.hpp`, which are real-time-hot and must
inline.

---

## The mechanism

### Blueprint → schedule → instance

The central separation. A **blueprint** is an immutable description of a graph.
Compiling it yields a **compiled schedule** — execution order, buffer
assignments, per-node call counts — computed *once*. An **instance** is one live
voice: a single arena allocation holding its buffers and its DSP state.

```
graph_blueprint  ──compile()──▶  compiled_schedule  ──instantiate()──▶  graph_instance
   (topology)                     (order, buffers)                          (a voice)
        │                                │                                      │
  described once                   computed once                        spawned N times
```

Many instances share one blueprint and one schedule. Spawning copies no topology
and does no graph work — it takes an arena and initializes state. That is what
makes hundreds of simultaneous voices cheap.

### Nodes are stateless code

A node *type* is `const` code. It describes its I/O, its control ports, and how
many bytes of state it needs — but never owns mutable DSP state:

```cpp
void process(void* state, const node_processing_context& ctx) const override;
```

Per-instance state lives in that instance's arena and is handed back on every
call. This is why one `oscillator` object can serve 200 voices, each with its own
phase.

### Hybrid ports

A control port is **control-rate by default, audio-rate when something is
connected to it**. Node code never branches on this — it reads through one view:

```cpp
ctx.params[0].at(i)     // per-sample value, whatever the port's rate
ctx.params[0].first()   // just the block's value, if that's all you need
```

Control-rate values arrive as a click-free linear ramp from the previous block's
value to the new target, so `set()` never zippers. Connect an LFO to the same
port and it silently becomes a per-sample audio-rate stream.

### Multi-rate: call-count scheduling

A node declares its output rate as a ratio of its input rate (`up2` is 2/1,
`down2` is 1/2). The compiler propagates these along the edges to give every node
a *region rate*, and the scheduler simply **calls a node as many times per block
as its region demands**. Oversampling a nonlinearity is one word:

```rune
over(2, shaper)     // runs `shaper` at 2x: up2 : shaper : down2
```

Rate conversion is a proper half-band FIR — the upsampler zero-stuffs and
filters, the downsampler filters then decimates. No interpolation.

A second, independent axis is **block size**: `downbloc` makes a region run at a
finer block (more calls, same sample rate), which is what buffering and spectral
nodes need.

### Parallelism

Per-instance: each active voice is a task, workers and the audio thread drain a
lock-free MPMC queue, and an atomic counter is the block barrier. Instances own
disjoint arenas and the master sum happens single-threaded afterwards, so
**parallel output is bit-identical to sequential**.

```cpp
rt.init({.workers = 4});     // 0 = single-threaded
```

### Real-time safety

The audio thread does not allocate. That isn't a claim, it's a test: a global
`operator new` trap is armed during `process()`, and a 500-block stress run
(parameter automation + spawn/kill + rewiring, 4 workers) asserts **zero**
allocations. Denormals are flushed (FTZ/DAZ) on every audio thread. Commands
reach the audio thread over a lock-free queue, and voices are addressed by
`{slot, generation}` handles — so a stale handle is dropped, never applied to a
recycled voice.

---

## Using the C++ API

Everything below lives in `include/xrune/api.hpp`.

### Describe a graph

The builder is fluent, addresses nodes by name, and accumulates errors rather
than throwing:

```cpp
#include <xrune/api.hpp>
using namespace xrune;

blueprint_builder synth("synth");
synth
    .add<oscillator>("osc", 440.0)
    .add<oscillator>("lfo", 3.0)
    .add<gain>("amp", 0.25)
    .connect("osc", 0, "amp", 0)         // audio: osc -> amp
    .modulate("lfo", 0, "amp", "gain")   // audio-rate LFO -> the gain port
    .output("amp");
```

A builder owns its nodes (`unique_ptr`), so it is move-only — bind it to a named
`blueprint_builder` as above, or register the chain in one expression:
`rt.register_blueprint(build("synth").add<gain>("g", 0.5).output("g"));`

### Run it

```cpp
runtime rt;
rt.init({.sample_rate = 48000, .block_size = 128, .output_channels = 2});
rt.start();

blueprint_id id = rt.register_blueprint(synth);
voice v = rt.spawn(id);
```

Block size **must be a power of two** — `init()` refuses otherwise, because the
64-byte buffer alignment guarantee depends on it.

### Change parameters

By name, or pre-resolved when you'll set it often:

```cpp
rt.set(v, "amp", "gain", 0.5);          // by name

param_ref g = rt.resolve(id, "amp", "gain");
rt.set(v, g, 0.5);                      // no lookup on the hot path
```

### Rune parameters

A rune's arguments are **compile-time** — they shape the graph, and are folded
into the port defaults. But Xrune records which ports each one fed, so a host
addresses them by **the name the user wrote**:

```cpp
const blueprint_info* bi = rt.describe(id);
for (const auto& p : bi->params)
    std::cout << p.name << " = " << p.default_value << "\n";   // f = 220, amp = 0.3

rt.set_param(v, "amp", 0.6);           // by rune-parameter name, smoothed
```

**Spawn a voice at a value, with no glide:**

```cpp
spawn_options o;
o.params = {{"f", 440.0}};             // this voice starts AT 440
rt.spawn(id, o);
```

That matters: setting a port *after* spawn ramps from the compiled default over
the first block — correct smoothing for changing a live voice, but a chirp on a
note-on. `spawn_options::params` writes the value before the voice ever runs.

**The honest caveat.** A parameter can only follow the ports it was passed to
*directly* (`sine(freq = f)`, `osc * amp`). If it was also used inside an
expression (`sine(freq = f * 2)`), that use was folded to a constant and cannot
track the parameter at run time. Xrune reports this instead of letting you find
out by ear — `rune_param_info::partial`, and `xrune -e` prints it:

```
tone  (4 nodes, 1 output terminal(s))
    f = 220   [partial: also folded into an expression]
    amp = 0.3
    det = 1.01   [drives no port: folded into an expression]
```

### Voice lifetimes

Voices can clean up after themselves, so you don't have to track them:

```cpp
spawn_options o;
o.life = {lifetime_kind::timed, rt.blocks(2.0)};   // reaped after 2 seconds
rt.spawn(id, o);
```

`permanent` (default) · `timed` · `until_finished` · `until_silent`. Reaped
voices are recycled; `rt.pump()` collects them on the control thread.

### Routing between voices

Voices aren't just leaves — they route into each other. Spawn a bus, then spawn
synths *into* it:

```cpp
voice bus = rt.spawn(reverb_bus);         // bus -> master
spawn_options into; into.into = bus;
voice v = rt.spawn(synth, into);          // synth -> bus

rt.unroute(v, bus);                       // rewire at runtime,
rt.route_to_master(v);                    // while it plays
```

A blueprint exposes named **terminals** (`input_terminal` / `output_terminal`),
so routing addresses a port of a voice, not merely "the voice".

### Introspection

`rt.describe(id)` returns the node list, every port with its range and default,
and the terminals — enough for a host (Idyl, a GUI, a tool) to discover a patch
it has never seen.

### Offline rendering

Swap the backend; nothing else changes:

```cpp
auto ob = std::make_unique<offline_backend>();
offline_backend* p = ob.get();
rt.use_backend(std::move(ob));
rt.init({}); rt.start();

p->render(200);                  // 200 blocks, no audio device
double level = p->rms(0);
```

---

## The Xrune language

The language shares the project's name: an Xrune program *is* an audio graph.
Files are `.rune`. It takes its connection algebra from Faust, its small surface
from Lua, and its `… end` blocks from Ruby.

```cpp
#include <xrune/lang/compile.hpp>
auto r = lang::load_file(rt, "examples/drone.rune");   // parse + lower + register
```

### Runes and sigils

A **rune** is a blueprint. A **sigil** is a reusable fragment — expanded at
compile time, so it costs nothing at run time.

``` xrune
sigil detuned(f, spread)
  sine(freq = f) , sine(freq = f * (1 + spread))
end

rune drone(base = 110)
  out detuned(base, 0.006) :> gain(0.12)
end
```

Both take parameters, with optional defaults. Arithmetic on them (`f * (1 +
spread)`) is evaluated at compile time.

### Nodes

```rune
sine(freq = 440)      // named argument
sine(440)             // positional
amp = gain(0.5)       // bind a name, to refer to it later
```

### The connection algebra

Four operators, Faust's. **`,` binds tightest; `:>` loosest.**

| Operator | Meaning | Arity rule |
|---|---|---|
| `A , B` | **parallel** — stack side by side | none; arities add |
| `A : B` | **sequential** — A's outputs into B's inputs, in order | `outs(A) == ins(B)` |
| `A <: B` | **split** — duplicate A's outputs to fill B's inputs | `ins(B) % outs(A) == 0` |
| `A :> B` | **merge** — *sum* groups of A's outputs into B's inputs | `outs(A) % ins(B) == 0` |

So `sine , sine :> gain` parses as `(sine , sine) :> gain` — two oscillators
summed into one gain. Arity mismatches are compile errors, not silent surprises.

### Arithmetic on signals

`+ - * /` work on signals as well as on compile-time numbers. The **operator**
picks the operation; the **kinds of the operands** pick the lowering:

```rune
sine * 0.5            // scale     -> gain(0.5)
0.5 * sine            // same, commutative
sine(220) + sine(330) // sum       -> add
a - b                 // subtract  -> add + inv
a / b                 // divide    -> div  (a/0 yields 0, never NaN)
-sine                 // negate    -> inv
```

A **bare number in signal position becomes a constant (DC) signal**, so `1 - env`
and `(sine , 0.5) :> mul` just work.

Signal ops are **element-wise across channels**, and a mono operand **broadcasts**:
`(l , r) * 0.5` scales both. Mismatched channel counts are a compile error. (This
is a deliberate divergence from Faust, where `A + B` means `(A,B) : +` and does not
broadcast.)

`%` stays compile-time only — there is no modulo node.

Arithmetic binds **tighter** than the connection algebra (`*` > `+` > `,` > `:`),
so `sine , sine * 0.5` is `sine , (sine * 0.5)`.

This is what makes modulation readable. A signal connected to a port *replaces*
its value rather than offsetting it, so the centre value has to be part of the
modulating signal:

```rune
(330 + lfo * 4) ~> osc.freq     // vibrato around 330 Hz
```

### The `_` and `!` wires

```rune
a , b , c , d :> _     // `_` is the identity wire: sums everything to one channel
sine : _ : gain(0.5)   // ... and is a no-op in sequence
(l , r) : (_ , !)      // `!` discards a channel — here, keep left, drop right
```

`_` lowers to a pass-through node, so it costs one buffer copy today.

### Modulation and explicit wiring

```rune
lfo ~> amp.gain       // audio-rate signal into a control port
a[1] -> b[0]          // an explicit wire, when the algebra is the wrong tool
```

### Terminals

```rune
input in(channels = 2)   // a bus other voices can be routed into
out mix                  // this voice's output
out send as aux          // a second, named output terminal
```

### Multi-rate

`over(n, E)` runs any expression `E` at n× the sample rate:

```rune
over(2, shaper)       // up2 : shaper : down2
```

There is no built-in saturator — `shaper` is whatever you supply (your own node,
or a Faust one). `over` is about the *scheduling*, not the DSP.

> `finer(n, …)`, the block-size counterpart, is **specified but not implemented**
> — it needs the `upbloc` node. Using it is a clean compile error, not a silent
> misbehaviour.

### Putting it together

```rune
// examples/drone.rune
sigil detuned(f, spread)
  sine(freq = f) , sine(freq = f * (1 + spread))
end

rune drone(base = 110)
  low  = detuned(base, 0.006) :> gain(0.12)
  high = detuned(base * 2, 0.01) :> gain(0.06)
  out (low , high) :> m2s
end
```

### Editor support

Syntax highlighting for `.rune` files lives in [editors/](editors/):

```bash
cd editors/vim    && ./install.sh              # Vim / Neovim (Linux, macOS)
cd editors/vscode && ./build_vsix.sh --install # VS Code / VSCodium
```

Comments are `// …` and `/* … */`. Newlines terminate statements (suppressed inside
brackets). Errors carry line, column and a message.

---

## Node vocabulary

The names the language and the JSON loader use, from `lang::standard_registry()`. Add
your own with `registry.add(name, factory)` and it becomes a language word
*and* JSON-loadable, for free.

| | |
|---|---|
| **sources** | `sine(freq)` · `noise` · `constant(value)` |
| **level** | `gain(gain)` · `fader(volume)` · `pan(pan)` · `inv` · `sinv` |
| **mixing** | `mix(inputs)` · `smix(inputs)` · `add` · `mul` · `div` · `adapt(inputs, outputs)` |
| **channels** | `m2s` · `s2m` · `bus(channels)` |
| **multi-rate** | `up2` · `down2` · `downbloc` |
| **spectral** | `stft(size)` · `stft_fwd(size, channels)` · `stft_bwd(size, channels)` |
| **misc** | `sah(rate)` · `counter` · `cut` (the `!` wire) |

---

## Serialization & graph export

A blueprint round-trips through JSON — and a reloaded patch renders
**bit-identical audio**:

```cpp
#include <xrune/serialize.hpp>

std::string text = to_json(bp, "patch");                 // save

graph_blueprint re; std::string err;
from_json(text, lang::standard_registry(), re, err);    // load
```

Nodes are rebuilt through the node registry, so anything registered — Faust and
future host nodes included — is loadable with no extra work.

Graphs export to Graphviz:

```cpp
std::string dot = to_dot(bp, "patch");   // dot -Tsvg patch.dot -o patch.svg
```

Audio edges are solid; audio-rate modulation is a dashed edge labelled with the
port it drives; rate-changing nodes are highlighted as region boundaries.

---

## Faust nodes

Two ways to host [Faust](https://faust.grame.fr) DSP, both behind CMake options:

- **static** (`XRUNE_WITH_FAUST`) — `faust_static<Dsp>` wraps a Faust-generated
  C++ class. No runtime dependency; the DSP is compiled into your binary.
- **JIT** (`XRUNE_WITH_FAUST_LLVM`) — `faust_jit` compiles a `.dsp` at runtime
  through libfaust/LLVM.

Faust parameters become ordinary Xrune ports, so they smooth, modulate and
serialize like any other. Register one as a DSL word and it's an Xrune node.

> If the JIT segfaults inside `getNumInputs()`, your `libfaust.so` doesn't match
> the Faust headers you built against — an ABI mismatch, not an Xrune bug.

---

## Status

**Working**: the engine, graph/instancing, hybrid ports, multi-rate,
per-instance parallelism, RT-safety, FFT/STFT nodes, the language front-end, the
C++ API, JSON/DOT serialization, Faust hosting.

**Not yet**: `upbloc` (and therefore `finer`), Csound and libsndfile hosts,
node-level parallelism with chain fusion, pooled spawn, push-based voice-end
events.

---

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).

Copyright (C) 2026 Johann Philippe

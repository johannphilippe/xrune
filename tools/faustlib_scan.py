#!/usr/bin/env python3
"""
faustlib_scan.py — extract a machine-readable index of the Faust standard
libraries, so Xrune's `faustlib("ve.korg35LPF")` node knows a function's audio
arity and its control parameters without a human writing it down.

The libraries carry structured documentation, which is what we mine:

    //------------------`(ve.)korg35LPF`-----------------
    // #### Usage
    // ```
    // _ : korg35LPF(normFreq,Q) : _          <- audio arity + parameter NAMES
    // ```
    // Where:
    // * `normFreq`: normalized frequency (0-1)
    // #### Test
    // ```
    // ... ve.korg35LPF(
    //       hslider("korg35LPF:normFreq", 0.35, 0, 1, 0.001),   <- REAL ranges,
    //       hslider("korg35LPF:Q", 3.5, 0.7, 10, 0.1)           <- author-chosen
    //     );
    // ```

The Test block is the valuable part: those defaults/min/max/step are the library
author's own, not something we invented.

Usage:
    ./faustlib_scan.py [--libdir DIR] [-o faustlib.json] [--stats]

Output JSON:
    { "ve.korg35LPF": { "ins": 1, "outs": 1,
                        "params": [ {"name","default","min","max","step","doc"} ],
                        "lib": "vaeffects.lib" }, ... }
"""

import argparse
import glob
import json
import os
import re
import sys

# `(ve.)korg35LPF` — the section header. Some are `name[n]` or contain markup.
RE_HEADER = re.compile(r"^//-+`\((\w+)\.\)(\w+)`?-*", re.M)

# A fenced block inside comments: lines of `// ...` between a `// ```` pair.
def comment_blocks(lines, start, end):
    """Yield each ``` fenced block (as a list of stripped lines) in lines[start:end]."""
    out, cur, inside = [], [], False
    for raw in lines[start:end]:
        if not raw.lstrip().startswith("//"):
            continue
        s = raw.lstrip()[2:].strip()
        if s.startswith("```"):
            if inside:
                out.append(cur)
                cur, inside = [], False
            else:
                inside = True
            continue
        if inside:
            cur.append(s)
    return out


# Usage lines are heterogeneous -- `_ : korg35LPF(normFreq,Q) : _` but also
# `hslider(...) : smoo;` -- so parsing audio arity out of prose is unreliable.
# We do NOT: libfaust reports getNumInputs()/getNumOutputs() authoritatively once
# the generated code is compiled. All we need from the docs is the ARGUMENT LIST,
# which we find by searching for the function's own name (known from the header).

RE_HSLIDER = re.compile(
    r'[hv](?:slider|bargraph)\s*\(\s*"(?P<label>[^"]+)"'
    r'\s*,\s*(?P<def>[-\d.eE+]+)'
    r'\s*,\s*(?P<min>[-\d.eE+]+)'
    r'\s*,\s*(?P<max>[-\d.eE+]+)'
    r'\s*,\s*(?P<step>[-\d.eE+]+)\s*\)'
)

# `* `normFreq`: normalized frequency (0-1)`
RE_WHERE = re.compile(r"^\*\s*`(?P<name>\w+)`\s*:\s*(?P<doc>.*)$")

# Faust's docs mark compile-time arguments consistently. These CANNOT become an
# hslider -- `fi.lowpass(N, fc)` with N as a slider simply will not compile -- so
# the caller has to supply them as a literal:  faustlib("fi.lowpass", N = 3)
RE_CONST_DOC = re.compile(
    r"constant numerical expression|compile[- ]time|must be.*constant|"
    r"filter order|number of (poles|zeros|taps|stages|bands)|table size",
    re.I)


def parse_usage(block, fname):
    """Find `fname(a,b,c)` or a bare `fname` in the Usage block.

    Returns the argument-name list, or None if the function appears applied to a
    non-identifier expression we could not turn into a parameter.
    """
    call = re.compile(r"(?<![\w.])" + re.escape(fname) + r"\s*\(([^()]*)\)")
    bare = re.compile(r"(?<![\w.])" + re.escape(fname) + r"(?![\w(])")

    for line in block:
        line = line.strip().rstrip(";")
        if not line:
            continue
        m = call.search(line)
        if m:
            raw = m.group(1).strip()
            if not raw:
                return []
            args = [a.strip() for a in raw.split(",")]
            # Only plain identifiers can become parameters. `os.osc(440)` or
            # `lowpass(3, fc)` in an EXAMPLE line is a usage sample, not a
            # signature -- skip rather than invent a parameter called "440".
            if any(not re.fullmatch(r"\w+", a) for a in args):
                continue
            return args
        if bare.search(line):
            return []          # e.g. `hslider(...) : smoo;`  -> no arguments
    return None


def parse_file(path):
    name = os.path.basename(path)
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()

    # Locate every section header and the span of text belonging to it.
    heads = []
    for i, raw in enumerate(lines):
        m = RE_HEADER.match(raw)
        if m:
            heads.append((i, m.group(1), m.group(2)))

    out = {}
    for idx, (line_no, prefix, fname) in enumerate(heads):
        end = heads[idx + 1][0] if idx + 1 < len(heads) else len(lines)
        blocks = comment_blocks(lines, line_no, end)
        if not blocks:
            continue

        args = parse_usage(blocks[0], fname)
        if args is None:
            continue

        # Descriptions from the "Where:" bullets.
        docs = {}
        for raw in lines[line_no:end]:
            s = raw.lstrip()
            if not s.startswith("//"):
                continue
            m = RE_WHERE.match(s[2:].strip())
            if m:
                docs[m.group("name")] = m.group("doc")

        # Ranges from the Test block's hsliders. Labels look like "korg35LPF:Q"
        # or plain "Q"; match on the part after the last ':'.
        ranges = {}
        for blk in blocks[1:]:
            for m in RE_HSLIDER.finditer(" ".join(blk)):
                label = m.group("label").split(":")[-1].strip()
                try:
                    ranges[label] = {
                        "default": float(m.group("def")),
                        "min": float(m.group("min")),
                        "max": float(m.group("max")),
                        "step": float(m.group("step")),
                    }
                except ValueError:
                    pass

        params = []
        for a in args:
            p = {"name": a}
            doc = docs.get(a, "")
            if doc:
                p["doc"] = doc

            # A compile-time argument cannot be a slider.
            if RE_CONST_DOC.search(doc):
                p["const"] = True
                params.append(p)
                continue
            p["const"] = False

            r = ranges.get(a)
            if r:
                p.update(r)
                p["from_test"] = True     # the library author's own range
            else:
                # NO fabricated range. Inventing 0..1 for a cutoff in Hz would
                # make Xrune clamp 2000 Hz down to 1 -- silently, and audibly
                # wrong. A wide range never clamps incorrectly; the default is
                # the honest unknown, and the caller can override it.
                p.update({"default": 0.0, "min": -1e6, "max": 1e6, "step": 0.001})
                p["from_test"] = False
            params.append(p)

        out[f"{prefix}.{fname}"] = {
            "params": params,
            "lib": name,
        }
    return out


# ---------------------------------------------------------------------------
# Verification: don't trust the parser -- make Faust prove it.
#
# The doc-mining above is a best guess. This pass generates the source that
# faustlib() would emit and compiles it with the real `faust`. A function only
# ships in the index if it actually builds.
#
# Faust also CORRECTS us: when it says "must be an integer constant numerical
# expression", an argument we thought was a slider is really compile-time. Faust
# puts those first by convention (lowpass(N, fc), fft(N)), so we retry marking a
# growing prefix of the arguments const. That turns a regex guess into a fact.
# ---------------------------------------------------------------------------

def gen_source(name, params, const_prefix=0, const_value=3):
    args = []
    for i, p in enumerate(params):
        if p.get("const") or i < const_prefix:
            args.append(str(const_value))
        else:
            args.append('hslider("%s", %g, %g, %g, %g)'
                        % (p["name"], p["default"], p["min"], p["max"], p["step"]))
    call = name if not args else "%s(%s)" % (name, ",".join(args))
    return 'import("stdfaust.lib");\nprocess = %s;\n' % call


def faust_compiles(src):
    import subprocess, tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".dsp", delete=False) as fh:
        fh.write(src)
        path = fh.name
    try:
        r = subprocess.run(["faust", "-lang", "cpp", "-o", os.devnull, path],
                           capture_output=True, text=True, timeout=30)
        return r.returncode == 0, (r.stderr or "").strip().split("\n")[0][:120]
    except Exception as e:
        return False, str(e)[:120]
    finally:
        os.unlink(path)


def verify(index, jobs=1):
    from concurrent.futures import ThreadPoolExecutor

    def one(item):
        name, meta = item
        params = meta["params"]
        n_slider = sum(1 for p in params if not p.get("const"))

        # Try as-is, then with a growing prefix of arguments forced compile-time.
        for prefix in range(0, n_slider + 1):
            ok, err = faust_compiles(gen_source(name, params, const_prefix=prefix))
            if ok:
                if prefix:
                    # Faust corrected us: those leading args are compile-time.
                    k = 0
                    for p in params:
                        if p.get("const"):
                            continue
                        if k < prefix:
                            p["const"] = True
                            p.pop("default", None); p.pop("min", None)
                            p.pop("max", None); p.pop("step", None)
                            p.pop("from_test", None)
                            k += 1
                meta["verified"] = True
                return name, meta, None
        return name, meta, err

    good, bad = {}, {}
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for name, meta, err in ex.map(one, sorted(index.items())):
            if err is None:
                good[name] = meta
            else:
                bad[name] = err
    return good, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--libdir", default="/usr/local/share/faust",
                    help="directory holding the .lib files")
    ap.add_argument("-o", "--output", default="-", help="output JSON (default: stdout)")
    ap.add_argument("--stats", action="store_true", help="print a summary to stderr")
    ap.add_argument("--verify", action="store_true",
                    help="compile every function with `faust` and keep only what "
                         "actually builds (also lets Faust correct which args are "
                         "compile-time). Slow, but the index becomes trustworthy.")
    ap.add_argument("-j", "--jobs", type=int, default=8, help="parallel faust jobs")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.libdir, "*.lib")))
    if not files:
        sys.exit(f"faustlib_scan: no .lib files in {args.libdir}")

    index = {}
    for f in files:
        if os.path.basename(f) in ("all.lib", "stdfaust.lib"):
            continue  # re-export shims, no definitions of their own
        index.update(parse_file(f))

    rejected = {}
    if args.verify:
        before = len(index)
        index, rejected = verify(index, jobs=args.jobs)
        print(f"faustlib_scan: verified {len(index)}/{before} functions "
              f"({len(rejected)} rejected)", file=sys.stderr)

    text = json.dumps(index, indent=2, sort_keys=True)
    if args.output == "-":
        print(text)
    else:
        with open(args.output, "w") as fh:
            fh.write(text + "\n")
        print(f"faustlib_scan: wrote {args.output} ({len(index)} functions)", file=sys.stderr)

    if args.stats:
        n_par = sum(1 for v in index.values() if v["params"])
        ctrl = [p for v in index.values() for p in v["params"] if not p.get("const")]
        trusted = sum(1 for p in ctrl if p.get("from_test"))
        withconst = sum(1 for v in index.values()
                        if any(p.get("const") for p in v["params"]))
        ready = sum(1 for v in index.values()
                    if not any(p.get("const") for p in v["params"])
                    and all(p.get("from_test") for p in v["params"]))
        print(f"  functions                     : {len(index)}", file=sys.stderr)
        print(f"  pure processors (no args)     : {len(index) - n_par}", file=sys.stderr)
        print(f"  with a compile-time arg       : {withconst}  (need an explicit value)",
              file=sys.stderr)
        print(f"  control params, range from Test: {trusted}/{len(ctrl)}", file=sys.stderr)
        print(f"  usable with NO user input     : {ready}", file=sys.stderr)
        if rejected:
            print(f"  rejected (do not compile)     : {len(rejected)}", file=sys.stderr)


if __name__ == "__main__":
    main()

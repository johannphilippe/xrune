#include "galdr/parser/parser.hpp"
#include "test_util.hpp"
#include <string>

using namespace xrune::galdr;

static std::string expr_of_out(const rune_def& r, size_t idx) {
    std::string s;
    dump_expr(*r.body[idx]->expr_, s);
    return s;
}

int main() {
    // --- basic rune structure ---
    XR_RUN("basic rune");
    {
        auto r = parse(
            "rune synth\n"
            "  osc = sine(freq = 440)\n"
            "  amp = gain(0.25)\n"
            "  osc : amp\n"
            "  out amp\n"
            "end\n");
        XR_CHECK(r.ok());
        XR_CHECK(r.prog.runes.size() == 1);
        XR_CHECK(r.prog.sigils.empty());
        const rune_def& s = r.prog.runes[0];
        XR_CHECK(s.name == "synth");
        XR_CHECK(s.body.size() == 4);
        XR_CHECK(s.body[0]->k == stmt::kind::binding);
        XR_CHECK(s.body[0]->name == "osc");
        XR_CHECK(s.body[0]->value->k == expr::kind::call);
        XR_CHECK(s.body[0]->value->text == "sine");
        XR_CHECK(s.body[0]->value->args.size() == 1);
        XR_CHECK(s.body[0]->value->args[0].name == "freq");
        XR_CHECK(s.body[2]->k == stmt::kind::wire);       // osc : amp
        XR_CHECK(s.body[3]->k == stmt::kind::out);
    }

    // --- connection-algebra precedence (comma tightest, merge loosest) ---
    XR_RUN("precedence");
    {
        auto r = parse("rune t\n  out a , b : c\n  out d : e , f\n  out g <: h :> k\nend\n");
        XR_CHECK(r.ok());
        const rune_def& t = r.prog.runes[0];
        XR_CHECK(expr_of_out(t, 0) == "((a , b) : c)");   // comma binds tighter than ':'
        XR_CHECK(expr_of_out(t, 1) == "(d : (e , f))");
        XR_CHECK(expr_of_out(t, 2) == "((g <: h) :> k)"); // ':>' loosest
    }

    // --- parameterized rune + sigil + arithmetic in args ---
    XR_RUN("params + sigil");
    {
        auto r = parse(
            "sigil osc_pair(f, det)\n"
            "  sine(freq = f) , sine(freq = f * det)\n"
            "end\n"
            "rune fat(base = 110)\n"
            "  out osc_pair(base, 1.007) :> gain(0.3)\n"
            "end\n");
        XR_CHECK(r.ok());
        XR_CHECK(r.prog.sigils.size() == 1);
        XR_CHECK(r.prog.sigils[0].params.size() == 2);
        XR_CHECK(r.prog.sigils[0].params[0].name == "f");
        XR_CHECK(r.prog.runes.size() == 1);
        XR_CHECK(r.prog.runes[0].params.size() == 1);
        XR_CHECK(r.prog.runes[0].params[0].name == "base");
        XR_CHECK(r.prog.runes[0].params[0].default_value != nullptr);
        // f * det parsed as arithmetic inside the arg
        std::string s;
        dump_expr(*r.prog.sigils[0].body[0]->expr_, s);
        XR_CHECK(s == "(sine(freq=f) , sine(freq=(f * det)))");
    }

    // --- modulation, explicit wire, terminals ---
    XR_RUN("modulation / explicit / terminals");
    {
        auto r = parse(
            "rune m\n"
            "  input in(channels = 2)\n"
            "  amp = gain(1.0)\n"
            "  sine(freq = 0.4) ~> amp.gain\n"
            "  osc[0] -> filt[1]\n"
            "  out amp as send\n"
            "end\n");
        XR_CHECK(r.ok());
        const rune_def& m = r.prog.runes[0];
        XR_CHECK(m.body[0]->k == stmt::kind::input);
        XR_CHECK(m.body[0]->name == "in");
        XR_CHECK(m.body[0]->args[0].name == "channels");

        XR_CHECK(m.body[2]->k == stmt::kind::modulate);
        XR_CHECK(m.body[2]->source->k == expr::kind::call);
        XR_CHECK(m.body[2]->target.parts.size() == 2);
        XR_CHECK(m.body[2]->target.parts[0] == "amp");
        XR_CHECK(m.body[2]->target.parts[1] == "gain");

        XR_CHECK(m.body[3]->k == stmt::kind::explicit_wire);
        XR_CHECK(m.body[3]->source->k == expr::kind::select);
        XR_CHECK(m.body[3]->source->index == 0);
        XR_CHECK(m.body[3]->target_node == "filt");
        XR_CHECK(m.body[3]->target_input == 1);

        XR_CHECK(m.body[4]->k == stmt::kind::out);
        XR_CHECK(m.body[4]->terminal == "send");
    }

    // --- comments + line continuation after a binary operator ---
    XR_RUN("comments + continuation");
    {
        auto r = parse(
            "// header comment\n"
            "rune s /* inline */\n"
            "  out src <:\n"                                   // continues on next line
            "    (pan(pan = -0.5) , pan(pan = 0.5)) :> fader(volume = 0.8)\n"
            "end\n");
        XR_CHECK(r.ok());
        XR_CHECK(r.prog.runes.size() == 1);
        XR_CHECK(r.prog.runes[0].body.size() == 1);
        XR_CHECK(r.prog.runes[0].body[0]->k == stmt::kind::out);
    }

    // --- the full §15 example parses cleanly (parser is node-name agnostic) ---
    XR_RUN("fuller example");
    {
        auto r = parse(
            "sigil osc_pair(f, det)\n"
            "  sine(freq = f) , sine(freq = f * det)\n"
            "end\n"
            "rune pad(base = 110)\n"
            "  body_amp = gain(0.18)\n"
            "  body     = osc_pair(base, 1.007) :> body_amp\n"
            "  breath   = noise() : gain(0.02)\n"
            "  sine(freq = 0.4) ~> body_amp.gain\n"
            "  colored = over(2, saturate(drive = 2.5))\n"
            "  width   = pan(pan = 0.0)\n"
            "  out (body , breath) :> colored : width\n"
            "end\n"
            "rune verb_bus\n"
            "  input in(channels = 2)\n"
            "  out in : reverb(mix = 0.3) : fader(volume = 0.7)\n"
            "end\n");
        for (const auto& d : r.diags) std::cerr << "  diag: " << d.format() << "\n";
        XR_CHECK(r.ok());
        XR_CHECK(r.prog.sigils.size() == 1);
        XR_CHECK(r.prog.runes.size() == 2);
    }

    // --- error cases carry positions ---
    XR_RUN("errors");
    {
        auto miss_end = parse("rune bad\n  osc = sine()\n");
        XR_CHECK(!miss_end.ok());

        auto bad_mod = parse("rune m\n  x ~> foo\nend\n"); // target needs node.port
        XR_CHECK(!bad_mod.ok());

        auto bad_char = parse("rune m\n  a = @\nend\n");
        XR_CHECK(!bad_char.ok());

        auto top = parse("gain(1)\n"); // top level must be rune/sigil
        XR_CHECK(!top.ok());
    }

    XR_MAIN_REPORT();
}

declare name "faustgain";
// A trivial Faust program: one control port "gain" scaling one audio channel.
// Used by test_faust_static (generated at build time with:
//   faust -lang cpp -double -cn faustgain -o faustgain.hpp gain.dsp)
gain = hslider("gain", 0.5, 0.0, 2.0, 0.01);
process = _ * gain;

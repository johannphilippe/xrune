# ⬢ X̸rune

  ᚷ A real‑time audio engine for Idƴl programs. \
  ᚷ Gentle enough to be an execution engine for other programs too. 

## Components 

 ᚷ Engine : An audio engine featuring realtime DSP processing   \
 ᚷ Graph : The rune system, allowing to combine & connect several audio glyphs together  \
 ᚷ Node : The glyphs \
 ᚷ Module : A plugin system allowing to host some of the best audio platforms like Csound, Faust (...) \
 ᚷ Lang : An audio programming language designed to enable abstract, efficient & modern DSP code   

# Features 

 ᚷ Realtime audio playback \
 ᚷ Modern C++ API (Miniaudio base for now) \
 ᚷ CLI client for realtime playback features 

# To do

 ᚷ Fix graph issues (complex tests, & why remove duplicates) \
 ᚷ Enable realtime connection / disconnection in graph \
 ᚷ Move node declaration outside of combinator3000.h (node.h, base_nodes.h etc) \
 ᚷ Create custom memory allocator (with new overload ? ) \
  - Problem with nodes allocation : they should not do it on their own (only when entering a graph) \
  - Possibility to resize bloc size (Jack context) \
 ᚷ Check that nodes that doesn't require their own memory can just forward pointer of upstream \
  - (problem if same output of upstream is connected to several nodes : each can modify the data) \
 ᚷ Generalize threading protection : lock free     
 ᚷ Move utilities to a separated library (outside combinator3000)     
 ᚷ Csound playback engine : enabled at compile time (CMake)
 ᚷ Jack needs resampling when ksmps is a divisor of Jack bloc size 


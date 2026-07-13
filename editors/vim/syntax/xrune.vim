" Vim syntax file
" Language:    Xrune (Xrune audio graph DSL)
" Filenames:   *.rune
" License:     GPL-3.0-or-later
"
" Note on ordering: in Vim a later-defined item wins over an earlier one, and a
" `syn keyword` always outranks a `syn match`. Two consequences drive the layout
" below:
"   - Comments and strings are defined LAST, so the `/` of `//` is not stolen by
"     the arithmetic operator match.
"   - Port access (amp.gain) uses nextgroup, so the port name is not highlighted
"     as the node keyword that happens to share its name.

if exists('b:current_syntax')
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

" ── Numbers: 440, 0.25, 1e-3, 0x1F ──────────────────────────────────────────
syn match   xruneNumber       '\<0[xX]\x\+\>'
syn match   xruneFloat        '\<\d\+\.\d\+\([eE][-+]\?\d\+\)\?\>'
syn match   xruneFloat        '\<\d\+[eE][-+]\?\d\+\>'
syn match   xruneNumber       '\<\d\+\>'

" ── Keywords ────────────────────────────────────────────────────────────────
syn keyword xruneKeyword      rune sigil end
syn keyword xruneKeyword      input out as
syn keyword xruneBoolean      true false

" ── Builtin combinators: over(n, E) / finer(n, E) ───────────────────────────
syn keyword xruneCombinator   over finer

" ── The standard node vocabulary (xrune::standard_registry) ─────────────────
syn keyword xruneNode         sine noise constant
syn keyword xruneNode         gain fader pan inv sinv
syn keyword xruneNode         mix smix add mul adapt
syn keyword xruneNode         m2s s2m bus
syn keyword xruneNode         up2 down2 downbloc
syn keyword xruneNode         stft stft_fwd stft_bwd
syn keyword xruneNode         sah counter

" ── Arithmetic / assignment ─────────────────────────────────────────────────
" (defined before comments, so `//` still wins over the `/` here)
syn match   xruneOperator     '[+\-*/%]'
syn match   xruneAssign       '='

" ── The connection algebra ──────────────────────────────────────────────────
" Defined after the arithmetic match so the multi-char forms take precedence.
syn match   xruneSeq          ':'
syn match   xrunePar          ','
syn match   xruneSplit        '<:'
syn match   xruneMerge        ':>'
syn match   xruneWire         '->'
syn match   xruneModulate     '\~>'

" ── Port access on a node: amp.gain ─────────────────────────────────────────
" nextgroup keeps `gain` here from being coloured as the `gain` NODE keyword.
syn match   xruneDot          '\.' nextgroup=xrunePort
syn match   xrunePort         '[a-zA-Z_][a-zA-Z0-9_]*' contained

" ── Channel index: a[0] -> b[1] ─────────────────────────────────────────────
syn match   xruneIndex        '\[\d\+\]'

" ── A named argument inside a call: sine(freq = 440) ────────────────────────
syn match   xruneArgName      '\<[a-zA-Z_][a-zA-Z0-9_]*\ze\s*=[^=]'

" ── Rune / sigil name in a definition ───────────────────────────────────────
syn match   xruneDefName      '\(\<\(rune\|sigil\)\s\+\)\@<=[a-zA-Z_][a-zA-Z0-9_]*'

" ── Strings ─────────────────────────────────────────────────────────────────
syn region  xruneString       start='"' skip='\\"' end='"' contains=xruneEscape
syn match   xruneEscape       '\\.' contained

" ── Comments (LAST: highest priority, so `//` beats the `/` operator) ───────
syn keyword xruneTodo         TODO FIXME XXX NOTE HACK contained
syn match   xruneComment      '//.*$'               contains=xruneTodo,@Spell
syn region  xruneComment      start='/\*' end='\*/' contains=xruneTodo,@Spell

" ── Folding: rune/sigil … end ───────────────────────────────────────────────
syn region  xruneBlock
      \ start='^\s*\<\(rune\|sigil\)\>'
      \ end='^\s*\<end\>'
      \ transparent fold keepend

" ── Highlight links ─────────────────────────────────────────────────────────
hi def link xruneComment      Comment
hi def link xruneTodo         Todo
hi def link xruneString       String
hi def link xruneEscape       SpecialChar
hi def link xruneNumber       Number
hi def link xruneFloat        Float
hi def link xruneBoolean      Boolean
hi def link xruneKeyword      Keyword
hi def link xruneCombinator   Function
hi def link xruneNode         Type
hi def link xruneDefName      Identifier
hi def link xruneArgName      Identifier
hi def link xrunePort         Identifier
hi def link xruneDot          Delimiter
hi def link xruneIndex        Special

" The algebra is the heart of the language — each operator gets its own group so
" a colour scheme can distinguish them, with sane defaults.
hi def link xruneSeq          Operator
hi def link xrunePar          Operator
hi def link xruneSplit        Statement
hi def link xruneMerge        Statement
hi def link xruneModulate     PreProc
hi def link xruneWire         PreProc
hi def link xruneOperator     Operator
hi def link xruneAssign       Operator

let b:current_syntax = 'xrune'

let &cpo = s:cpo_save
unlet s:cpo_save

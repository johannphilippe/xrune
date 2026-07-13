" Vim syntax file
" Language:    Galdr (Xrune audio graph DSL)
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
syn match   galdrNumber       '\<0[xX]\x\+\>'
syn match   galdrFloat        '\<\d\+\.\d\+\([eE][-+]\?\d\+\)\?\>'
syn match   galdrFloat        '\<\d\+[eE][-+]\?\d\+\>'
syn match   galdrNumber       '\<\d\+\>'

" ── Keywords ────────────────────────────────────────────────────────────────
syn keyword galdrKeyword      rune sigil end
syn keyword galdrKeyword      input out as
syn keyword galdrBoolean      true false

" ── Builtin combinators: over(n, E) / finer(n, E) ───────────────────────────
syn keyword galdrCombinator   over finer

" ── The standard node vocabulary (galdr::standard_registry) ─────────────────
syn keyword galdrNode         sine noise constant
syn keyword galdrNode         gain fader pan inv sinv
syn keyword galdrNode         mix smix add mul adapt
syn keyword galdrNode         m2s s2m bus
syn keyword galdrNode         up2 down2 downbloc
syn keyword galdrNode         stft stft_fwd stft_bwd
syn keyword galdrNode         sah counter

" ── Arithmetic / assignment ─────────────────────────────────────────────────
" (defined before comments, so `//` still wins over the `/` here)
syn match   galdrOperator     '[+\-*/%]'
syn match   galdrAssign       '='

" ── The connection algebra ──────────────────────────────────────────────────
" Defined after the arithmetic match so the multi-char forms take precedence.
syn match   galdrSeq          ':'
syn match   galdrPar          ','
syn match   galdrSplit        '<:'
syn match   galdrMerge        ':>'
syn match   galdrWire         '->'
syn match   galdrModulate     '\~>'

" ── Port access on a node: amp.gain ─────────────────────────────────────────
" nextgroup keeps `gain` here from being coloured as the `gain` NODE keyword.
syn match   galdrDot          '\.' nextgroup=galdrPort
syn match   galdrPort         '[a-zA-Z_][a-zA-Z0-9_]*' contained

" ── Channel index: a[0] -> b[1] ─────────────────────────────────────────────
syn match   galdrIndex        '\[\d\+\]'

" ── A named argument inside a call: sine(freq = 440) ────────────────────────
syn match   galdrArgName      '\<[a-zA-Z_][a-zA-Z0-9_]*\ze\s*=[^=]'

" ── Rune / sigil name in a definition ───────────────────────────────────────
syn match   galdrDefName      '\(\<\(rune\|sigil\)\s\+\)\@<=[a-zA-Z_][a-zA-Z0-9_]*'

" ── Strings ─────────────────────────────────────────────────────────────────
syn region  galdrString       start='"' skip='\\"' end='"' contains=galdrEscape
syn match   galdrEscape       '\\.' contained

" ── Comments (LAST: highest priority, so `//` beats the `/` operator) ───────
syn keyword galdrTodo         TODO FIXME XXX NOTE HACK contained
syn match   galdrComment      '//.*$'               contains=galdrTodo,@Spell
syn region  galdrComment      start='/\*' end='\*/' contains=galdrTodo,@Spell

" ── Folding: rune/sigil … end ───────────────────────────────────────────────
syn region  galdrBlock
      \ start='^\s*\<\(rune\|sigil\)\>'
      \ end='^\s*\<end\>'
      \ transparent fold keepend

" ── Highlight links ─────────────────────────────────────────────────────────
hi def link galdrComment      Comment
hi def link galdrTodo         Todo
hi def link galdrString       String
hi def link galdrEscape       SpecialChar
hi def link galdrNumber       Number
hi def link galdrFloat        Float
hi def link galdrBoolean      Boolean
hi def link galdrKeyword      Keyword
hi def link galdrCombinator   Function
hi def link galdrNode         Type
hi def link galdrDefName      Identifier
hi def link galdrArgName      Identifier
hi def link galdrPort         Identifier
hi def link galdrDot          Delimiter
hi def link galdrIndex        Special

" The algebra is the heart of the language — each operator gets its own group so
" a colour scheme can distinguish them, with sane defaults.
hi def link galdrSeq          Operator
hi def link galdrPar          Operator
hi def link galdrSplit        Statement
hi def link galdrMerge        Statement
hi def link galdrModulate     PreProc
hi def link galdrWire         PreProc
hi def link galdrOperator     Operator
hi def link galdrAssign       Operator

let b:current_syntax = 'galdr'

let &cpo = s:cpo_save
unlet s:cpo_save

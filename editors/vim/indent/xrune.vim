" Vim indent file for Xrune (Xrune audio graph DSL)
" Indent inside rune/sigil … end blocks.
" License: GPL-3.0-or-later

if exists('b:did_indent')
  finish
endif
let b:did_indent = 1

setlocal indentexpr=GetXruneIndent()
setlocal indentkeys=0=end,o,O,!^F

if exists('*GetXruneIndent')
  finish
endif

function! GetXruneIndent() abort
  let l:prev = prevnonblank(v:lnum - 1)
  if l:prev == 0
    return 0
  endif

  let l:prevline = getline(l:prev)
  let l:curline  = getline(v:lnum)
  let l:ind      = indent(l:prev)

  " Opening a block indents the body.
  if l:prevline =~# '^\s*\<\%(rune\|sigil\)\>'
    let l:ind += shiftwidth()
  endif

  " `end` closes it: line it back up with its rune/sigil.
  if l:curline =~# '^\s*\<end\>'
    let l:ind -= shiftwidth()
  endif

  return l:ind < 0 ? 0 : l:ind
endfunction

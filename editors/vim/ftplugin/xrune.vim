" Vim filetype plugin for Xrune (Xrune audio graph DSL)
" License: GPL-3.0-or-later

if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

let s:cpo_save = &cpo
set cpo&vim

" C-style comments: // line, /* block */
setlocal commentstring=//\ %s
setlocal comments=s1:/*,mb:*,ex:*/,://

" Xrune has no semicolons; statements are newline-terminated.
setlocal formatoptions-=t formatoptions+=croql

" Two-space indent, as in the examples.
setlocal expandtab
setlocal shiftwidth=2
setlocal softtabstop=2

" Fold rune/sigil … end blocks (syntax file defines the regions).
setlocal foldmethod=syntax
setlocal foldlevel=99

" Jump between `rune`/`sigil` and its `end` with %
if exists('loaded_matchit')
  let b:match_words = '\<\%(rune\|sigil\)\>:\<end\>'
endif

let b:undo_ftplugin = 'setlocal commentstring< comments< formatoptions<'
      \ . ' expandtab< shiftwidth< softtabstop< foldmethod< foldlevel<'

let &cpo = s:cpo_save
unlet s:cpo_save

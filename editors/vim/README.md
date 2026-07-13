# Galdr — Vim / Neovim support

Syntax highlighting, folding and indentation for Galdr (`.rune`), the audio-graph
DSL of [Xrune](../../README.md).

## Install

```bash
./install.sh              # auto-detect Vim / Neovim, prompt if both are present
./install.sh --vim        # Vim only        (~/.vim)
./install.sh --nvim       # Neovim only     (~/.config/nvim)
./install.sh --both       # both, no prompt
./install.sh --uninstall  # remove
```

Works on Linux and macOS. Then open any `.rune` file.

> **Re-run `install.sh` after pulling.** It *copies* the files, so an older
> install keeps shadowing the repo (`~/.vim` comes before most other entries in
> `runtimepath`).

If you prefer a plugin manager, point it at this directory instead — no copying,
always current:

```vim
Plug '/path/to/xrune/editors/vim'                " vim-plug
```
```lua
{ dir = '/path/to/xrune/editors/vim' }           -- lazy.nvim
```

## What you get

| File | Purpose |
|---|---|
| `syntax/galdr.vim` | highlighting |
| `ftdetect/galdr.vim` | `*.rune` → `filetype=galdr` |
| `ftplugin/galdr.vim` | `//` comments, 2-space indent, folding, `%` matching |
| `indent/galdr.vim` | auto-indent inside `rune`/`sigil` … `end` |

**Highlighted:** keywords (`rune` `sigil` `end` `input` `out` `as`), the node
vocabulary (`sine` `gain` `mix` …) as types, the combinators `over`/`finer`,
numbers, strings, and C-style comments (`//`, `/* */`).

The **connection algebra** gets a group per operator, so a colour scheme can
tell them apart:

| Operator | Group | Default |
|---|---|---|
| `,` parallel | `galdrPar` | Operator |
| `:` sequential | `galdrSeq` | Operator |
| `<:` split | `galdrSplit` | Statement |
| `:>` merge | `galdrMerge` | Statement |
| `~>` modulate | `galdrModulate` | PreProc |
| `->` explicit wire | `galdrWire` | PreProc |

Override them in your vimrc:

```vim
hi galdrMerge    ctermfg=214 guifg=#fabd2f
hi galdrModulate ctermfg=142 guifg=#b8bb26
```

## Editing

- `za` — fold/unfold a `rune` or `sigil`
- `%` — jump between `rune`/`sigil` and its `end` (needs `matchit`)
- `gcc` — comment a line, if you have a commenter plugin (`commentstring` is set)

## Note on port names

`amp.gain` highlights `gain` as a **port**, not as the `gain` node — the syntax
file uses `nextgroup` after the `.` so a port that shares a node's name isn't
mis-coloured.

# Editor support for Galdr

Syntax highlighting for `.rune` files — the Galdr audio-graph DSL of
[Xrune](../README.md).

| Editor | Install |
|---|---|
| [Vim / Neovim](vim/) | `cd vim && ./install.sh` |
| [VS Code](vscode/) | `cd vscode && ./build_vsix.sh --install` |

Both cover the same ground: keywords, the node vocabulary, the connection
algebra (`,` `:` `<:` `:>` `~>` `->`), numbers, strings, C-style comments
(`//`, `/* */`), and folding on `rune`/`sigil` … `end`.

# Editor support for Xrune

Syntax highlighting for `.rune` files — the [Xrune](../README.md) language.

| Editor | Install |
|---|---|
| [Vim / Neovim](vim/) | `cd vim && ./install.sh` |
| [VS Code](vscode/) | `cd vscode && ./build_vsix.sh --install` |

Both cover the same ground: keywords, the node vocabulary, the connection
algebra (`,` `:` `<:` `:>` `~>` `->`), numbers, strings, C-style comments
(`//`, `/* */`), and folding on `rune`/`sigil` … `end`.

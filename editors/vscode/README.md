# Xrune — VS Code support

Syntax highlighting for the [Xrune](../../README.md) language (`.rune` files).

## Build & install

```bash
./build_vsix.sh             # -> xrune-lang-0.1.0.vsix
./build_vsix.sh --install   # build, then install into VS Code
./build_vsix.sh --clean     # remove built .vsix files
```

A `.vsix` is only a ZIP with a particular layout, so the script needs **no npm,
no node and no vsce** — just `zip`. It writes the `extension.vsixmanifest` and
`[Content_Types].xml` itself and reads the version straight from
`xrune-lang/package.json`.

Install by hand instead, if you prefer:

```bash
code --install-extension xrune-lang-0.1.0.vsix
```

or in VS Code: **Extensions → ⋯ → Install from VSIX…**

Works with VS Code, VS Code Insiders and VSCodium (the script finds whichever
`code` CLI you have).

## Layout

```
xrune-lang/
  package.json                     extension manifest
  language-configuration.json      comments, brackets, indent, folding
  syntaxes/xrune.tmLanguage.json   the TextMate grammar
build_vsix.sh                      packager
```

## What you get

- **Highlighting** — keywords (`rune` `sigil` `end` `input` `out` `as`), the node
  vocabulary (`sine` `gain` `mix` …), the combinators `over`/`finer`, numbers,
  strings, and the connection algebra (`,` `:` `<:` `:>` `~>` `->`), each with its
  own scope.
- **Comments** — `//` and `/* */`, so `Ctrl+/` works.
- **Folding & indent** — on `rune`/`sigil` … `end`.
- **Bracket matching** and auto-closing for `(` `[` `"`.

### Scopes

Themeable via `editor.tokenColorCustomizations`:

| Element | Scope |
|---|---|
| `rune` `sigil` `end` … | `keyword.control.xrune` |
| node names | `support.class.node.xrune` |
| `over` / `finer` | `support.function.combinator.xrune` |
| `:>` merge | `keyword.operator.merge.xrune` |
| `<:` split | `keyword.operator.split.xrune` |
| `~>` modulate | `keyword.operator.modulate.xrune` |
| `->` wire | `keyword.operator.wire.xrune` |
| port after `.` | `variable.other.property.xrune` |
| rune/sigil name | `entity.name.function.xrune` |

```jsonc
"editor.tokenColorCustomizations": {
  "textMateRules": [
    { "scope": "keyword.operator.modulate.xrune",
      "settings": { "foreground": "#b8bb26", "fontStyle": "bold" } }
  ]
}
```

## Bumping the version

Edit `version` in `xrune-lang/package.json` and re-run `./build_vsix.sh`; the
output filename follows it automatically.

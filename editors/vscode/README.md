# Galdr — VS Code support

Syntax highlighting for Galdr (`.rune`), the audio-graph DSL of
[Xrune](../../README.md).

## Build & install

```bash
./build_vsix.sh             # -> galdr-lang-0.1.0.vsix
./build_vsix.sh --install   # build, then install into VS Code
./build_vsix.sh --clean     # remove built .vsix files
```

A `.vsix` is only a ZIP with a particular layout, so the script needs **no npm,
no node and no vsce** — just `zip`. It writes the `extension.vsixmanifest` and
`[Content_Types].xml` itself and reads the version straight from
`galdr-lang/package.json`.

Install by hand instead, if you prefer:

```bash
code --install-extension galdr-lang-0.1.0.vsix
```

or in VS Code: **Extensions → ⋯ → Install from VSIX…**

Works with VS Code, VS Code Insiders and VSCodium (the script finds whichever
`code` CLI you have).

## Layout

```
galdr-lang/
  package.json                     extension manifest
  language-configuration.json      comments, brackets, indent, folding
  syntaxes/galdr.tmLanguage.json   the TextMate grammar
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
| `rune` `sigil` `end` … | `keyword.control.galdr` |
| node names | `support.class.node.galdr` |
| `over` / `finer` | `support.function.combinator.galdr` |
| `:>` merge | `keyword.operator.merge.galdr` |
| `<:` split | `keyword.operator.split.galdr` |
| `~>` modulate | `keyword.operator.modulate.galdr` |
| `->` wire | `keyword.operator.wire.galdr` |
| port after `.` | `variable.other.property.galdr` |
| rune/sigil name | `entity.name.function.galdr` |

```jsonc
"editor.tokenColorCustomizations": {
  "textMateRules": [
    { "scope": "keyword.operator.modulate.galdr",
      "settings": { "foreground": "#b8bb26", "fontStyle": "bold" } }
  ]
}
```

## Bumping the version

Edit `version` in `galdr-lang/package.json` and re-run `./build_vsix.sh`; the
output filename follows it automatically.

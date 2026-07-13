#!/usr/bin/env bash
# build_vsix.sh — Build (and optionally install) the galdr-lang VS Code extension.
#
# A .vsix is just a ZIP with a specific layout, so this needs no npm, no node and
# no vsce — only `zip`, which every Unix has.
#
# Usage:
#   ./build_vsix.sh              # build galdr-lang-<version>.vsix
#   ./build_vsix.sh --install    # build, then install into VS Code
#   ./build_vsix.sh --clean      # remove built .vsix files
#
# The version is read from galdr-lang/package.json.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT_DIR="$SCRIPT_DIR/galdr-lang"

if [[ -t 1 ]]; then
  GRN='\033[0;32m' YEL='\033[1;33m' BLU='\033[0;34m' RED='\033[0;31m' NC='\033[0m'
else
  GRN='' YEL='' BLU='' RED='' NC=''
fi
ok()   { printf "${GRN}✓${NC} %s\n" "$*"; }
info() { printf "${BLU}→${NC} %s\n" "$*"; }
warn() { printf "${YEL}!${NC} %s\n" "$*"; }
die()  { printf "${RED}✗${NC} %s\n" "$*" >&2; exit 1; }

# Files packaged into the extension (relative to galdr-lang/).
FILES=(
  "package.json"
  "language-configuration.json"
  "syntaxes/galdr.tmLanguage.json"
)

# Read a top-level string field out of package.json without needing jq.
pkg_field() {
  local key="$1"
  sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" \
      "$EXT_DIR/package.json" | head -1
}

# The VS Code engine version lives nested under "engines" — grab it separately.
pkg_engine() {
  sed -n 's/.*"vscode"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
      "$EXT_DIR/package.json" | head -1
}

# ── Arguments ────────────────────────────────────────────────────────────────
INSTALL=0
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --install) INSTALL=1 ;;
    --clean)   CLEAN=1 ;;
    -h|--help)
      awk 'FNR==1{next} /^[^#]/{exit} {sub(/^# ?/,""); print}' "$0"
      exit 0
      ;;
    *) die "Unknown option: $arg  (use --help for usage)" ;;
  esac
done

if [[ $CLEAN -eq 1 ]]; then
  rm -f "$SCRIPT_DIR"/*.vsix
  ok "Removed built .vsix files."
  exit 0
fi

command -v zip >/dev/null || die "'zip' not found. Install it (apt install zip / brew install zip)."
[[ -d "$EXT_DIR" ]] || die "Extension directory not found: $EXT_DIR"

NAME="$(pkg_field name)"
DISPLAY="$(pkg_field displayName)"
DESC="$(pkg_field description)"
VERSION="$(pkg_field version)"
PUBLISHER="$(pkg_field publisher)"
ENGINE="$(pkg_engine)"

[[ -n "$NAME" && -n "$VERSION" && -n "$PUBLISHER" ]] \
  || die "Could not read name/version/publisher from package.json"

VSIX="$SCRIPT_DIR/${NAME}-${VERSION}.vsix"

echo "Galdr VS Code extension builder"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
info "$DISPLAY $VERSION  (publisher: $PUBLISHER)"

# ── Stage the package in a temp dir ──────────────────────────────────────────
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/extension"
for rel in "${FILES[@]}"; do
  [[ -f "$EXT_DIR/$rel" ]] || die "missing file: galdr-lang/$rel"
  mkdir -p "$STAGE/extension/$(dirname "$rel")"
  cp -f "$EXT_DIR/$rel" "$STAGE/extension/$rel"
  ok "  packaged $rel"
done
[[ -f "$SCRIPT_DIR/README.md" ]] && cp -f "$SCRIPT_DIR/README.md" "$STAGE/extension/README.md"

# ── [Content_Types].xml — required by the OPC (Open Packaging) format ────────
cat > "$STAGE/[Content_Types].xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension=".json" ContentType="application/json" />
  <Default Extension=".md" ContentType="text/markdown" />
  <Default Extension=".vsixmanifest" ContentType="text/xml" />
</Types>
XML

# ── extension.vsixmanifest — how VS Code identifies the package ──────────────
cat > "$STAGE/extension.vsixmanifest" <<XML
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0"
  xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011"
  xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Description xml:space="preserve">${DESC}</Description>
    <DisplayName>${DISPLAY}</DisplayName>
    <Identity Language="en-US" Id="${NAME}" Version="${VERSION}" Publisher="${PUBLISHER}" />
    <Tags>galdr,rune,xrune,audio,dsl,syntax</Tags>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="${ENGINE}" />
    </Properties>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true" />
  </Assets>
</PackageManifest>
XML

# ── Zip it up ────────────────────────────────────────────────────────────────
rm -f "$VSIX"
( cd "$STAGE" && zip -q -r "$VSIX" . )

echo ""
ok "Built $(basename "$VSIX")  ($(wc -c < "$VSIX") bytes)"

# ── Optionally install ───────────────────────────────────────────────────────
if [[ $INSTALL -eq 1 ]]; then
  echo ""
  CODE=""
  for c in code code-insiders codium; do
    if command -v "$c" &>/dev/null; then CODE="$c"; break; fi
  done
  [[ -n "$CODE" ]] || die "No 'code' CLI found. In VS Code: Cmd/Ctrl+Shift+P → 'Shell Command: Install code command in PATH'."

  info "Installing with $CODE …"
  "$CODE" --install-extension "$VSIX" --force
  ok "Installed. Reload VS Code, then open a .rune file."
else
  echo ""
  echo "  Install with:   ./build_vsix.sh --install"
  echo "  Or manually:    code --install-extension $(basename "$VSIX")"
  echo "  Or in VS Code:  Extensions → ⋯ → Install from VSIX…"
fi

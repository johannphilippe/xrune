#!/usr/bin/env bash
# install.sh — Install the Xrune Vim plugin (syntax highlighting for *.rune)
#
# Works on Linux and macOS, with Vim, Neovim, or both.
#
# Usage:
#   ./install.sh               # auto-detect editors, prompt if both found
#   ./install.sh --vim         # install for Vim only
#   ./install.sh --nvim        # install for Neovim only
#   ./install.sh --both        # install for both without prompting
#   ./install.sh --uninstall   # remove installed files
#
# Installs:
#   syntax/xrune.vim     — syntax highlighting
#   ftdetect/xrune.vim   — filetype detection for *.rune
#   ftplugin/xrune.vim   — comments, indent width, folding, matchit
#   indent/xrune.vim     — auto-indent inside rune/sigil … end

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Terminal colours ─────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
  GRN='\033[0;32m' YEL='\033[1;33m' BLU='\033[0;34m' RED='\033[0;31m' NC='\033[0m'
else
  GRN='' YEL='' BLU='' RED='' NC=''
fi

ok()   { printf "${GRN}✓${NC} %s\n" "$*"; }
info() { printf "${BLU}→${NC} %s\n" "$*"; }
warn() { printf "${YEL}!${NC} %s\n" "$*"; }
die()  { printf "${RED}✗${NC} %s\n" "$*" >&2; exit 1; }

# ── Files to install (relative to editors/vim/) ──────────────────────────────
FILES=(
  "syntax/xrune.vim"
  "ftdetect/xrune.vim"
  "ftplugin/xrune.vim"
  "indent/xrune.vim"
)

# Files from the pre-rename plugin (the language was called "Galdr"). They must
# be removed, or the stale syntax/ftdetect keeps shadowing the new ones.
LEGACY_FILES=(
  "syntax/galdr.vim"
  "ftdetect/galdr.vim"
  "ftplugin/galdr.vim"
  "indent/galdr.vim"
)

install_to() {
  local dst_root="$1" label="$2"
  info "Installing into $dst_root  ($label)"

  for rel in "${LEGACY_FILES[@]}"; do
    if [[ -f "$dst_root/$rel" ]]; then
      rm -f "$dst_root/$rel"
      warn "  removed legacy $rel"
    fi
  done

  local any=0
  for rel in "${FILES[@]}"; do
    local src="$SCRIPT_DIR/$rel"
    local dst="$dst_root/$rel"
    if [[ ! -f "$src" ]]; then
      warn "  source not found: $rel — skipping"
      continue
    fi
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
    ok "  $rel"
    any=1
  done
  [[ $any -eq 1 ]] && ok "$label install complete."
  return 0
}

uninstall_from() {
  local dst_root="$1" label="$2"
  info "Removing from $dst_root  ($label)"

  local any=0
  for rel in "${FILES[@]}" "${LEGACY_FILES[@]}"; do
    local dst="$dst_root/$rel"
    if [[ -f "$dst" ]]; then
      rm -f "$dst"
      ok "  removed $rel"
      any=1
    fi
  done
  [[ $any -eq 0 ]] && warn "  nothing to remove in $dst_root"
  return 0
}

# ── Detect editor runtime directories ────────────────────────────────────────
detect_dirs() {
  local -n _vim_ref=$1
  local -n _nvim_ref=$2

  # Vim: ~/.vim on both Linux and macOS
  if command -v vim &>/dev/null || [[ -d "$HOME/.vim" ]]; then
    _vim_ref="$HOME/.vim"
  fi

  # Neovim: follows XDG on both Linux and macOS
  local nvim_cfg="${XDG_CONFIG_HOME:-$HOME/.config}/nvim"
  if command -v nvim &>/dev/null || [[ -d "$nvim_cfg" ]]; then
    _nvim_ref="$nvim_cfg"
  fi
}

choose_editor() {
  local vim_dir="$1" nvim_dir="$2"
  echo ""
  echo "Both Vim and Neovim detected."
  echo "  [1] Vim only       ($vim_dir)"
  echo "  [2] Neovim only    ($nvim_dir)"
  echo "  [3] Both"
  echo ""
  printf "Install for: [1/2/3] "
  read -r choice </dev/tty
  case "$choice" in
    1) echo "vim"  ;;
    2) echo "nvim" ;;
    3) echo "both" ;;
    *) die "Invalid choice." ;;
  esac
}

# ── Arguments ────────────────────────────────────────────────────────────────
MODE=""
UNINSTALL=0

for arg in "$@"; do
  case "$arg" in
    --vim)       MODE=vim  ;;
    --nvim)      MODE=nvim ;;
    --both)      MODE=both ;;
    --uninstall) UNINSTALL=1 ;;
    -h|--help)
      awk 'FNR==1{next} /^[^#]/{exit} {sub(/^# ?/,""); print}' "$0"
      exit 0
      ;;
    *) die "Unknown option: $arg  (use --help for usage)" ;;
  esac
done

# ── Main ─────────────────────────────────────────────────────────────────────
echo "Xrune Vim plugin installer"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

VIM_DIR=""
NVIM_DIR=""
detect_dirs VIM_DIR NVIM_DIR

# An explicitly requested editor gets its default dir even if not auto-detected.
if [[ "$MODE" == "vim"  || "$MODE" == "both" ]] && [[ -z "$VIM_DIR"  ]]; then
  VIM_DIR="$HOME/.vim"
fi
if [[ "$MODE" == "nvim" || "$MODE" == "both" ]] && [[ -z "$NVIM_DIR" ]]; then
  NVIM_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/nvim"
fi

if [[ -z "$VIM_DIR" && -z "$NVIM_DIR" ]]; then
  die "Neither Vim nor Neovim detected. Install vim or nvim, then re-run."
fi

if [[ -z "$MODE" ]]; then
  if [[ -n "$VIM_DIR" && -n "$NVIM_DIR" ]]; then
    MODE="$(choose_editor "$VIM_DIR" "$NVIM_DIR")"
  elif [[ -n "$VIM_DIR" ]]; then
    MODE=vim
  else
    MODE=nvim
  fi
fi

if [[ $UNINSTALL -eq 1 ]]; then
  echo ""
  [[ "$MODE" == "vim"  || "$MODE" == "both" ]] && uninstall_from "$VIM_DIR"  "Vim"
  [[ "$MODE" == "nvim" || "$MODE" == "both" ]] && uninstall_from "$NVIM_DIR" "Neovim"
  echo ""
  ok "Uninstall complete."
  exit 0
fi

echo ""
[[ "$MODE" == "vim"  || "$MODE" == "both" ]] && install_to "$VIM_DIR"  "Vim"
[[ "$MODE" == "nvim" || "$MODE" == "both" ]] && { echo ""; install_to "$NVIM_DIR" "Neovim"; }

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ok "Installation complete."
echo ""
echo "  Open any .rune file to get syntax highlighting."
echo "  Fold a rune with  za  ;  jump rune <-> end with  %  (needs matchit)."
echo ""
echo "  Alternative: point a plugin manager at editors/vim/"
echo "    vim-plug:  Plug '$SCRIPT_DIR'"
echo "    lazy.nvim: { dir = '$SCRIPT_DIR' }"

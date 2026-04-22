#!/bin/sh
# Install bowie — the bowie language interpreter.
# Usage: curl --proto '=https' --tlsv1.2 -sSf \
#   https://raw.githubusercontent.com/bowie-lang/bowie/master/install.sh | sh
set -e

REPO="bowie-lang/bowie"
BINARY="bowie"
INSTALL_DIR="${BOWIE_INSTALL_DIR:-/usr/local/bin}"

say() { printf '\033[1mbowie installer:\033[0m %s\n' "$*"; }
err() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ── OS detection ──────────────────────────────────────────────────────────────
os=$(uname -s 2>/dev/null | tr '[:upper:]' '[:lower:]')
case "$os" in
  darwin) os="macos" ;;
  linux)  os="linux" ;;
  *) err "Unsupported operating system: $os" ;;
esac

# ── Architecture detection ────────────────────────────────────────────────────
arch=$(uname -m 2>/dev/null)
case "$arch" in
  x86_64 | amd64)   arch="amd64" ;;
  aarch64 | arm64)  arch="arm64" ;;
  *) err "Unsupported architecture: $arch" ;;
esac

# ── Availability check ────────────────────────────────────────────────────────
if [ "$os" = "macos" ] && [ "$arch" = "amd64" ]; then
  err "Pre-built binaries for macOS/x86_64 are not yet available.
       Please build from source: https://github.com/$REPO"
fi

TARGET="${BINARY}-${os}-${arch}"
URL="https://github.com/${REPO}/releases/latest/download/${TARGET}"

say "Detected platform: ${os}/${arch}"
say "Downloading from: ${URL}"

# ── Download ──────────────────────────────────────────────────────────────────
TMP=$(mktemp)
trap 'rm -f "$TMP"' EXIT

if command -v curl > /dev/null 2>&1; then
  curl --proto '=https' --tlsv1.2 -sSfL "$URL" -o "$TMP" \
    || err "Download failed. Check that a release exists for ${os}/${arch}."
elif command -v wget > /dev/null 2>&1; then
  wget --https-only -qO "$TMP" "$URL" \
    || err "Download failed. Check that a release exists for ${os}/${arch}."
else
  err "curl or wget is required to download bowie."
fi

chmod +x "$TMP"

# ── Install ───────────────────────────────────────────────────────────────────
if [ -w "$INSTALL_DIR" ]; then
  mv "$TMP" "${INSTALL_DIR}/${BINARY}"
elif command -v sudo > /dev/null 2>&1; then
  say "Installing to ${INSTALL_DIR} (sudo required)..."
  sudo mv "$TMP" "${INSTALL_DIR}/${BINARY}"
else
  FALLBACK_DIR="$HOME/.local/bin"
  mkdir -p "$FALLBACK_DIR"
  mv "$TMP" "${FALLBACK_DIR}/${BINARY}"
  INSTALL_DIR="$FALLBACK_DIR"
  say "Installed to ${INSTALL_DIR}/${BINARY} (no sudo available)."
  case ":$PATH:" in
    *":${FALLBACK_DIR}:"*) ;;
    *) say "Add ${FALLBACK_DIR} to your PATH to use bowie." ;;
  esac
fi

say "Installed: ${INSTALL_DIR}/${BINARY}"

if command -v bowie > /dev/null 2>&1; then
  say "Version: $(bowie --version 2>/dev/null || echo 'unknown')"
fi

say "Done! Run 'bowie --help' to get started."

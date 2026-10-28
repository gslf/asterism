#!/bin/sh
# asterism installer for Linux and macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/gslf/asterism/main/install.sh | sh
#   ./install.sh                  inside a clone: builds this checkout
#
# It installs the asterism executable (a release archive, or a build from
# source) under ~/.local, then runs "asterism setup", which picks the models
# for this machine, asks for the Telegram bot token, pairs your account and
# starts the agent as a user service.
#
# Options:
#   --from-source     build from source even when a release archive exists
#   --prefix DIR      install under DIR (default ~/.local)
#   --root DIR        engine root passed to setup (default ~/asterism)
#   --no-setup        install only
#   --yes             accept every default without asking
#   --uninstall       stop the service, remove the program (keeps ~/asterism)
#   --purge           with --uninstall: also delete the engine root
#   -- ARGS           pass the rest to asterism setup (e.g. -- --no-service)
#
# ASTERISM_REPO=owner/name selects another GitHub repository.
set -eu

REPO=${ASTERISM_REPO:-gslf/asterism}
PREFIX=$HOME/.local
ROOT=
FROM_SOURCE=0
SETUP=1
YES=0
UNINSTALL=0
PURGE=0
SETUP_ARGS=

say() { printf '%s\n' "$*"; }
die() { printf 'asterism install: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --from-source) FROM_SOURCE=1 ;;
    --prefix) [ $# -gt 1 ] || die "--prefix needs a directory"; shift; PREFIX=$1 ;;
    --prefix=*) PREFIX=${1#--prefix=} ;;
    --root) [ $# -gt 1 ] || die "--root needs a directory"; shift; ROOT=$1 ;;
    --root=*) ROOT=${1#--root=} ;;
    --no-setup) SETUP=0 ;;
    --yes|-y) YES=1 ;;
    --uninstall) UNINSTALL=1 ;;
    --purge) PURGE=1 ;;
    --) shift; SETUP_ARGS=$*; break ;;
    --help|-h) sed -n '2,22p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
  shift
done

# Questions go to the terminal even when this script arrives through a pipe.
TTY=
if [ "$YES" -eq 0 ] && [ -r /dev/tty ] && (: </dev/tty) 2>/dev/null; then TTY=/dev/tty; fi

ask() { # ask "question" default(y|n)
  [ -n "$TTY" ] || { [ "$2" = y ]; return; }
  if [ "$2" = y ]; then printf '%s [Y/n] ' "$1"; else printf '%s [y/N] ' "$1"; fi
  read -r answer <"$TTY" || answer=
  case "${answer:-$2}" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

have() { command -v "$1" >/dev/null 2>&1; }

BIN=$PREFIX/bin/asterism

# ── uninstall ────────────────────────────────────────────────────────────
if [ "$UNINSTALL" -eq 1 ]; then
  if [ -x "$BIN" ]; then
    "$BIN" setup --remove-service ${ROOT:+--root "$ROOT"} || true
  fi
  rm -f -- "$BIN"
  rm -rf -- "$PREFIX/share/asterism"
  say "asterism removed from $PREFIX"
  DATA=${ROOT:-$HOME/asterism}
  if [ "$PURGE" -eq 1 ] && [ -d "$DATA" ]; then
    if [ -n "$TTY" ]; then
      printf 'Delete %s with its memory, sessions, models and secrets? Type "delete": ' "$DATA"
      read -r answer <"$TTY" || answer=
    else
      answer=
      say "--purge needs a terminal to confirm; $DATA is kept"
    fi
    if [ "$answer" = delete ]; then rm -rf -- "$DATA"; say "$DATA deleted"; fi
  else
    say "your data stays in $DATA"
  fi
  exit 0
fi

# ── platform ─────────────────────────────────────────────────────────────
case "$(uname -s)" in
  Linux) OS=linux ;;
  Darwin) OS=macos ;;
  *) die "this script is for Linux and macOS; on Windows use install.ps1" ;;
esac
case "$(uname -m)" in
  x86_64|amd64) ARCH=x86_64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) ARCH=$(uname -m) ;;
esac
PLATFORM=$OS-$ARCH

# Run as ./install.sh inside a clone: that checkout is the source.
HERE=
case "$0" in
  */install.sh|install.sh)
    HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
    [ -f "$HERE/CMakeLists.txt" ] && [ -d "$HERE/asngn" ] || HERE= ;;
esac
[ -n "$HERE" ] && FROM_SOURCE=1

# A release build runs models on the CPU; an NVIDIA GPU wants a CUDA build.
if [ "$FROM_SOURCE" -eq 0 ] && have nvidia-smi && have nvcc; then
  if ask "An NVIDIA GPU and CUDA are here. Build from source to use the GPU?" y; then
    FROM_SOURCE=1
  fi
fi

TMP=$(mktemp -d 2>/dev/null || mktemp -d -t asterism)
trap 'rm -rf -- "$TMP"' EXIT INT TERM

fetch() { # fetch URL FILE
  if have curl; then curl -fsSL --retry 3 -o "$2" "$1"
  elif have wget; then wget -q -O "$2" "$1"
  else die "needs curl or wget"
  fi
}

sha256() {
  if have sha256sum; then sha256sum "$1" | awk '{print $1}'
  else shasum -a 256 "$1" | awk '{print $1}'
  fi
}

# ── release archive ──────────────────────────────────────────────────────
install_release() {
  asset=asterism-$PLATFORM.tar.gz
  url=https://github.com/$REPO/releases/latest/download/$asset
  say "Downloading $asset"
  fetch "$url" "$TMP/$asset" 2>/dev/null || return 1
  fetch "$url.sha256" "$TMP/$asset.sha256" || die "no checksum for $asset"
  want=$(awk '{print $1}' "$TMP/$asset.sha256")
  [ "$(sha256 "$TMP/$asset")" = "$want" ] || die "$asset does not match its checksum"
  mkdir -p "$TMP/x" "$PREFIX"
  tar -xzf "$TMP/$asset" -C "$TMP/x" --strip-components=1
  [ -x "$TMP/x/bin/asterism" ] || die "$asset holds no bin/asterism"
  mkdir -p "$PREFIX/bin" "$PREFIX/share"
  rm -rf -- "$PREFIX/share/asterism"
  cp -R "$TMP/x/share/asterism" "$PREFIX/share/asterism"
  cp "$TMP/x/bin/asterism" "$BIN.new" && mv -f "$BIN.new" "$BIN"
  if [ "$OS" = macos ]; then xattr -d com.apple.quarantine "$BIN" 2>/dev/null || true; fi
  return 0
}

# ── build from source ────────────────────────────────────────────────────
missing_tools() {
  miss=
  for t in git cmake cc c++ make; do have "$t" || miss="$miss $t"; done
  if [ "$OS" = linux ] && ! { have curl-config || pkg-config --exists libcurl 2>/dev/null ||
       [ -f /usr/include/curl/curl.h ] || [ -f /usr/include/x86_64-linux-gnu/curl/curl.h ] ||
       [ -f /usr/include/aarch64-linux-gnu/curl/curl.h ]; }; then
    miss="$miss libcurl-headers"
  fi
  printf '%s' "$miss"
}

install_command() {
  if [ "$OS" = macos ]; then
    have cc || { say "xcode-select --install"; return; }
    say "brew install cmake git"
  elif have apt-get; then say "sudo apt-get install -y git cmake build-essential libcurl4-openssl-dev"
  elif have dnf; then say "sudo dnf install -y git cmake gcc gcc-c++ make libcurl-devel"
  elif have pacman; then say "sudo pacman -S --needed --noconfirm git cmake base-devel curl"
  elif have zypper; then say "sudo zypper install -y git cmake gcc gcc-c++ make libcurl-devel"
  elif have apk; then say "sudo apk add git cmake build-base curl-dev"
  fi
}

cmake_new_enough() {
  v=$(cmake --version 2>/dev/null | awk 'NR==1{print $3}')
  major=${v%%.*}; rest=${v#*.}; minor=${rest%%.*}
  [ "${major:-0}" -gt 3 ] || { [ "${major:-0}" -eq 3 ] && [ "${minor:-0}" -ge 16 ]; }
}

install_source() {
  miss=$(missing_tools)
  if [ -n "$miss" ]; then
    cmd=$(install_command)
    say "Missing to build asterism:$miss"
    [ -n "$cmd" ] || die "install them with your package manager, then run this again"
    if ask "Run: $cmd ?" y; then
      sh -c "$cmd" || die "the install command failed"
    else
      die "install them, then run this again"
    fi
    miss=$(missing_tools)
    [ -z "$miss" ] || die "still missing:$miss"
  fi
  cmake_new_enough || die "CMake 3.16 or newer is needed"

  if [ -n "$HERE" ]; then
    SRC=$HERE
  else
    SRC=${XDG_CACHE_HOME:-$HOME/.cache}/asterism/src
    if [ -d "$SRC/.git" ]; then
      say "Updating $SRC"
      git -C "$SRC" pull --ff-only
    else
      say "Cloning github.com/$REPO"
      mkdir -p "$(dirname -- "$SRC")"
      git clone "https://github.com/$REPO.git" "$SRC"
    fi
  fi
  if [ -d "$SRC/.git" ] || [ -f "$SRC/.git" ]; then
    git -C "$SRC" submodule update --init --recursive
  fi

  set -- -DCMAKE_BUILD_TYPE=Release -DASTERISM_BUILD_TESTS=OFF
  if [ "$OS" = macos ] && [ "$ARCH" = arm64 ]; then
    set -- "$@" -DGGML_METAL=ON
  elif have nvcc; then
    say "CUDA found: building with GPU support"
    set -- "$@" -DGGML_CUDA=ON
  fi
  jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
  say "Building asterism (a few minutes)"
  cmake -S "$SRC" -B "$SRC/build-install" "$@"
  cmake --build "$SRC/build-install" -j "$jobs"
  cmake --install "$SRC/build-install" --prefix "$PREFIX"
}

# An update replaces the program: stop the agent first, start it again after.
WAS_RUNNING=0
if [ -x "$BIN" ] && "$BIN" status ${ROOT:+--root "$ROOT"} 2>/dev/null | grep -q "is running"; then
  WAS_RUNNING=1
  "$BIN" stop ${ROOT:+--root "$ROOT"} >/dev/null 2>&1 || true
fi

if [ "$FROM_SOURCE" -eq 0 ]; then
  if ! install_release; then
    say "No release archive for $PLATFORM: building from source"
    install_source
  fi
else
  install_source
fi
say "Installed: $("$BIN" --version) in $BIN"

# ── PATH ─────────────────────────────────────────────────────────────────
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *)
    case "${SHELL:-}" in
      */zsh) RC=$HOME/.zshrc ;;
      */bash) RC=$HOME/.bashrc ;;
      */fish) RC=$HOME/.config/fish/config.fish ;;
      *) RC=$HOME/.profile ;;
    esac
    case "$RC" in
      *.fish) LINE="fish_add_path $PREFIX/bin" ;;
      *) LINE="export PATH=\"$PREFIX/bin:\$PATH\"" ;;
    esac
    if ask "Add $PREFIX/bin to PATH in $RC?" y; then
      mkdir -p "$(dirname -- "$RC")"
      printf '\n# asterism\n%s\n' "$LINE" >>"$RC"
      say "Open a new terminal (or: $LINE) to use asterism by name"
    else
      say "Add this to your shell profile: $LINE"
    fi ;;
esac

# ── setup ────────────────────────────────────────────────────────────────
# shellcheck disable=SC2086 # SETUP_ARGS: plain flags, split on purpose
if [ "$SETUP" -eq 1 ]; then
  if [ -n "$TTY" ]; then
    "$BIN" setup ${ROOT:+--root "$ROOT"} $SETUP_ARGS <"$TTY"
  else
    "$BIN" setup --yes ${ROOT:+--root "$ROOT"} $SETUP_ARGS
  fi
else
  say "Next: asterism setup"
fi
if [ "$WAS_RUNNING" -eq 1 ] && ! "$BIN" status ${ROOT:+--root "$ROOT"} | grep -q "is running"; then
  "$BIN" start ${ROOT:+--root "$ROOT"}
fi

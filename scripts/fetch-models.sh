#!/bin/sh
# Verified reference-model installer: downloads one tier of
# models.manifest.tsv, pinned by revision and checked by size and SHA-256.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=${HOME}/asterism
ROOT_SET=0
INSTALL_TOOLS=0
REPAIR=0
# legacy matches the engine's built-in defaults; other tiers need
# examples/<tier>.xcdn as <engine-root>/config.xcdn.
TIER=legacy

usage() {
  echo "usage: scripts/fetch-models.sh [engine-root] [--tier legacy|base|standard|plus] [--repair] [--install-tools]"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --install-tools) INSTALL_TOOLS=1 ;;
    --repair) REPAIR=1 ;;
    --tier)
      [ $# -gt 1 ] || { usage >&2; exit 2; }
      shift
      TIER=$1 ;;
    --tier=*) TIER=${1#--tier=} ;;
    --help) usage; exit 0 ;;
    --*) echo "unknown option: $1" >&2; exit 2 ;;
    *)
      if [ "$ROOT_SET" -eq 1 ]; then
        echo "only one engine-root may be supplied" >&2; exit 2
      fi
      ROOT=$1
      ROOT_SET=1 ;;
  esac
  shift
done

case "$TIER" in
  legacy|base|standard|plus) ;;
  *) echo "unknown tier: $TIER (legacy, base, standard, plus)" >&2; exit 2 ;;
esac

DIR=$ROOT/models
SOURCES=$DIR/.sources
mkdir -p "$DIR" "$SOURCES"

hash_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

verify() {
  [ -f "$1" ] || return 1
  [ "$(wc -c < "$1" | tr -d ' ')" = "$2" ] || return 1
  [ "$(hash_file "$1")" = "$3" ]
}

COUNT=0
while IFS="	" read -r tiers name size sha url post; do
  case "$tiers" in ''|'#'*) continue ;; esac
  case ",$tiers," in *",$TIER,"*) ;; *) continue ;; esac
  COUNT=$((COUNT + 1))
  dest=$DIR/$name
  if [ "$post" = "-" ] && verify "$dest" "$size" "$sha"; then
    echo "verified: $dest"
    continue
  fi
  if [ "$post" = "-" ] && [ -e "$dest" ] && [ "$REPAIR" -ne 1 ]; then
    echo "verification failed: $dest (use --repair to replace)" >&2
    exit 1
  fi
  if [ "$post" = "-" ]; then
    rm -f -- "$dest.part"
    echo "downloading verified model: $name"
    curl -L --fail --retry 3 -o "$dest.part" "$url"
    verify "$dest.part" "$size" "$sha" || {
      echo "download verification failed: $name" >&2; exit 1;
    }
    mv -f -- "$dest.part" "$dest"
    echo "installed: $dest"
    continue
  fi

  source=$SOURCES/$name
  if ! verify "$source" "$size" "$sha"; then
    if [ -e "$source" ] && [ "$REPAIR" -ne 1 ]; then
      echo "verification failed: $source (use --repair to replace)" >&2
      exit 1
    fi
    rm -f -- "$source.part"
    echo "downloading verified source: $name"
    curl -L --fail --retry 3 -o "$source.part" "$url"
    verify "$source.part" "$size" "$sha" || {
      echo "download verification failed: $name" >&2; exit 1;
    }
    mv -f -- "$source.part" "$source"
  fi

  cp -- "$source" "$dest.part"
  key=${post%%=*}
  value=${post#*=}
  python3 "$HERE/gguf_add_kv.py" "$dest.part" "$key" "$value"
  mv -f -- "$dest.part" "$dest"
  echo "installed: $dest"
done < "$HERE/models.manifest.tsv"

[ "$COUNT" -gt 0 ] || { echo "no models for tier: $TIER" >&2; exit 1; }
echo "tier $TIER: $COUNT verified models in $DIR"
if [ "$TIER" != legacy ]; then
  echo "engine config for this tier: $(dirname -- "$HERE")/examples/$TIER.xcdn -> $ROOT/config.xcdn"
fi

if [ "$INSTALL_TOOLS" -eq 1 ]; then
  PKG=$HERE/../build/packages
  TOOLS=$ROOT/tools
  [ -d "$PKG" ] || { echo "tool packages not found: $PKG" >&2; exit 1; }
  mkdir -p "$TOOLS"
  for src in "$PKG"/*; do
    [ -d "$src" ] || continue
    dst=$TOOLS/$(basename "$src")
    rm -rf -- "$dst"
    cp -R -- "$src" "$dst"
    echo "tool package: $(basename "$src")"
  done
fi

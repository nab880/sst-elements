#!/bin/bash
# Touch src/sst/elements/<name>/.ignore for all elements except carcosa, memHierarchy, vanadis (run from sst-elements root). Undo: -u/--undo.

set -e
ELEMENTS_DIR="$(cd "$(dirname "$0")/../src/sst/elements" && pwd)"
KEEP="carcosa memHierarchy vanadis"

undo=
if [ "${1:-}" = "-u" ] || [ "${1:-}" = "--undo" ]; then
  undo=1
fi

for dir in "$ELEMENTS_DIR"/*; do
  [ -d "$dir" ] || continue
  name=$(basename "$dir")
  case "$name" in
    Makefile.am|Makefile.in|README.md) continue ;;
  esac
  [ -f "$dir/Makefile.am" ] || continue

  if [ -n "$undo" ]; then
    if [ -f "$dir/.ignore" ]; then
      rm -f "$dir/.ignore"
      echo "Removed .ignore from $name"
    fi
  else
    keep=
    for k in $KEEP; do [ "$name" = "$k" ] && keep=1 && break; done
    if [ -z "$keep" ]; then
      touch "$dir/.ignore"
      echo "Added .ignore to $name"
    fi
  fi
done

if [ -z "$undo" ]; then
  echo
  echo "Done. Next steps:"
  echo "  ./autogen.sh"
  echo "  ./configure ..."
  echo "  make -j"
  echo
  echo "To restore building all elements, run: $0 --undo"
else
  echo
  echo "Done. Run ./autogen.sh then reconfigure to build all elements again."
fi

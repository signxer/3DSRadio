#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT=${TMPDIR:-/tmp}/3dsradio-search-test
cd "$ROOT"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$ROOT/include" \
  "$ROOT/tests/test_search_input.c" \
  "$ROOT/source/ime_pinyin.c" \
  "$ROOT/source/search_input.c" \
  -o "$OUT"
"$OUT"
rm -f "$OUT"

IME_OUT=${TMPDIR:-/tmp}/3dsradio-ime-test
cc -std=c11 -Wall -Wextra -Werror \
  -I"$ROOT/include" \
  "$ROOT/tests/test_ime_pinyin.c" \
  "$ROOT/source/ime_pinyin.c" \
  -o "$IME_OUT"
"$IME_OUT"
rm -f "$IME_OUT"
echo "host tests: OK"

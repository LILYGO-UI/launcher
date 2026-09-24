#!/bin/sh
set -eu

launcher=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/launcher-cli.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

set +e
"$launcher" --width 320 --height 240 >"$test_dir/output.log" 2>&1
status=$?
set -e

if [ "$status" -ne 2 ]; then
  cat "$test_dir/output.log" >&2
  echo "expected a horizontal base canvas to exit with status 2, got $status" >&2
  exit 1
fi

grep -F "must define a portrait base canvas" "$test_dir/output.log" >/dev/null

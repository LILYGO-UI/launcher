#!/bin/sh
set -eu

test_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_build=$(mktemp -d "${TMPDIR:-/tmp}/lilygo-display-session.XXXXXX")
trap 'rm -rf "$test_build"' EXIT HUP INT TERM
test_cxx=${1:-${CXX:-c++}}

for backend in software egl egl-legacy; do
    case "$backend" in
        software) set -- -DLV_LINUX_DRM_USE_EGL=0 ;;
        egl) set -- -DLV_LINUX_DRM_USE_EGL=1 -DLILYGO_UI_HAS_DRM_HANDOFF=1 ;;
        egl-legacy) set -- -DLV_LINUX_DRM_USE_EGL=1 ;;
    esac
    "$test_cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
        "$@" \
        -I"$test_root/tests/display_stubs" -I"$test_root/src" \
        "$test_root/tests/test_display_session.cpp" "$test_root/src/display_session.cpp" \
        -o "$test_build/display-session-$backend"
    "$test_build/display-session-$backend"
done

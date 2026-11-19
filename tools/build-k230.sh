#!/bin/sh
# Independently build the shared SDK, launcher and an optional sample app with
# the toolchain/sysroot belonging to the target's K230 Buildroot image.
set -eu
if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 BUILDROOT_OUTPUT APPKIT_SOURCE [APPLICATION_SOURCE]" >&2
    exit 2
fi
launcher=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk=$(CDPATH= cd -- "$1" && pwd)
appkit=$(CDPATH= cd -- "$2" && pwd)
application=
if [ "$#" -eq 3 ]; then
    application=$(CDPATH= cd -- "$3" && pwd)
fi
export LILYGO_UI_K230_SDK="$sdk"
toolchain="$appkit/cmake/k230-toolchain.cmake"
[ -f "$toolchain" ] || { echo "AppKit K230 toolchain missing: $toolchain" >&2; exit 1; }
[ -f "$sdk/host/share/buildroot/toolchainfile.cmake" ] || {
    echo "Build the T-Display-K230 image first; this directory lacks the Buildroot application SDK" >&2
    exit 1
}
command -v lpm >/dev/null || { echo "Install lilygo-lpm on the Linux build host first" >&2; exit 1; }
for command_name in cmake cpack; do
    command -v "$command_name" >/dev/null || { echo "Required build tool is missing: $command_name" >&2; exit 1; }
done
case "${BUILD_JOBS:-4}" in ''|0|*[!0-9]*) echo "BUILD_JOBS must be a positive integer" >&2; exit 2 ;; esac

validate_project() {
    project_source=$1
    [ -f "$project_source/CMakeLists.txt" ] || {
        echo "Not a top-level CMake project: $project_source" >&2; exit 1;
    }
    project_cache="$project_source/build/k230-cross/CMakeCache.txt"
    if [ -f "$project_cache" ]; then
        cached_sdk=$(sed -n 's/^LILYGO_UI_K230_SDK:PATH=//p' "$project_cache")
        cached_toolchain=$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:FILEPATH=//p' "$project_cache")
        [ -n "$cached_toolchain" ] || cached_toolchain=$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:UNINITIALIZED=//p' "$project_cache")
        if [ "$cached_sdk" != "$sdk" ] || [ "$cached_toolchain" != "$toolchain" ]; then
            echo "Build directory belongs to a different SDK/toolchain: $project_source/build/k230-cross" >&2
            echo "Move that directory aside before selecting another K230 SDK." >&2
            exit 1
        fi
    fi
}
validate_project "$appkit"
validate_project "$launcher"
[ -z "$application" ] || validate_project "$application"

build_project() {
    source=$1
    shift
    cmake -S "$source" -B "$source/build/k230-cross" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DLILYGO_UI_K230_SDK="$sdk" \
        -DLilyGoUI_DIR="$appkit" \
        -DLILYGO_UI_PLATFORM=k230 -DLILYGO_UI_TARGET_OS=buildroot -DLILYGO_UI_RENDERER=software \
        -DCM0_SIMULATOR=OFF -DBUILD_TESTING=OFF \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr "$@"
    cmake --build "$source/build/k230-cross" --parallel "${BUILD_JOBS:-4}"
    cpack --config "$source/build/k230-cross/CPackConfig.cmake" -G TGZ \
        -B "$source/dist/k230"
}

build_project "$appkit"
build_project "$launcher" -DLAUNCHER_SYSTEM_SESSION=OFF
if [ -n "$application" ]; then
    build_project "$application"
fi

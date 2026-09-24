#!/bin/sh
set -eu

helper=${1:?update helper path is required}
unit=${2:?update service path is required}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/launcher-update.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM

mock_bin="$test_root/bin"
state_dir="$test_root/state"
source_deb="$test_root/launcher.deb"
command_log="$test_root/commands.log"
installed_version_file="$test_root/installed-version"
sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
mkdir -p "$mock_bin"
: > "$command_log"
printf '1.0.0\n' > "$installed_version_file"
printf 'package payload\n' > "$source_deb"

cat > "$mock_bin/id" <<'EOF'
#!/bin/sh
test "${1:-}" = -u
printf '0\n'
EOF

cat > "$mock_bin/systemctl" <<'EOF'
#!/bin/sh
printf 'systemctl %s\n' "$*" >> "$LAUNCHER_UPDATE_TEST_LOG"
if [ "${1:-}" = is-active ]; then
    [ "${LAUNCHER_UPDATE_TEST_ACTIVE:-0}" = 1 ]
    exit
fi
exit 0
EOF

cat > "$mock_bin/dpkg-query" <<'EOF'
#!/bin/sh
case "$*" in
    *Status-Status*)
        printf 'installed'
        ;;
    *Version*)
        sed -n '1p' "$LAUNCHER_UPDATE_TEST_INSTALLED_VERSION"
        ;;
    *)
        exit 1
        ;;
esac
EOF

cat > "$mock_bin/dpkg-deb" <<'EOF'
#!/bin/sh
test "${1:-}" = --field
case "${3:-}" in
    Package)
        printf '%s\n' "${LAUNCHER_UPDATE_TEST_PACKAGE:-lilygo-ui-launcher}"
        ;;
    Version)
        printf '%s\n' "${LAUNCHER_UPDATE_TEST_VERSION:-2.0.0}"
        ;;
    Architecture)
        printf '%s\n' "${LAUNCHER_UPDATE_TEST_ARCHITECTURE:-arm64}"
        ;;
    *)
        exit 1
        ;;
esac
EOF

cat > "$mock_bin/sha256sum" <<'EOF'
#!/bin/sh
printf '%s  %s\n' "$LAUNCHER_UPDATE_TEST_SHA256" "${2:-}"
EOF

cat > "$mock_bin/dpkg" <<'EOF'
#!/bin/sh
case "${1:-}" in
    --print-architecture)
        printf 'arm64\n'
        ;;
    --compare-versions)
        [ "${LAUNCHER_UPDATE_TEST_ALLOW_UPGRADE:-1}" = 1 ]
        ;;
    --install)
        printf 'dpkg %s helper=%s\n' "$*" \
            "${LILYGO_UI_LAUNCHER_UPDATE_HELPER:-0}" >> "$LAUNCHER_UPDATE_TEST_LOG"
        if [ "${LAUNCHER_UPDATE_TEST_INSTALL_FAIL:-0}" = 1 ]; then
            exit 1
        fi
        printf '%s\n' "${LAUNCHER_UPDATE_TEST_VERSION:-2.0.0}" \
            > "$LAUNCHER_UPDATE_TEST_INSTALLED_VERSION"
        ;;
    *)
        exit 1
        ;;
esac
EOF

chmod +x "$mock_bin/id" "$mock_bin/systemctl" "$mock_bin/dpkg-query" \
    "$mock_bin/dpkg-deb" "$mock_bin/sha256sum" "$mock_bin/dpkg"

run_helper() {
    PATH="$mock_bin:$PATH" \
    LAUNCHER_UPDATE_TEST_LOG="$command_log" \
    LAUNCHER_UPDATE_TEST_INSTALLED_VERSION="$installed_version_file" \
    LAUNCHER_UPDATE_TEST_SHA256="$sha256" \
    LILYGO_UI_LAUNCHER_STATE_DIR="$state_dir" \
        sh "$helper" "$@"
}

run_helper queue "$source_deb" 2.0.0 "$sha256"
test -f "$state_dir/updates/pending/package.deb"
test "$(sed -n '1p' "$state_dir/updates/pending/version")" = 2.0.0
test "$(sed -n '1p' "$state_dir/updates/pending/sha256")" = "$sha256"
grep -Fx 'status=queued' "$state_dir/last-update" >/dev/null
grep -Fx 'systemctl start --no-block lilygo-ui-launcher-update.service' \
    "$command_log" >/dev/null

run_helper worker
test ! -e "$state_dir/updates/pending"
grep -Fx 'status=succeeded' "$state_dir/last-update" >/dev/null
grep -Fx 'version=2.0.0' "$state_dir/last-update" >/dev/null
grep -Fx 'message=updated' "$state_dir/last-update" >/dev/null
grep -F 'dpkg --install -- ' "$command_log" | grep -F ' helper=1' >/dev/null
grep -Fx 'systemctl restart lilygo-ui-launcher.service' "$command_log" >/dev/null
run_helper status | grep -Fx 'status=succeeded' >/dev/null

printf '1.0.0\n' > "$installed_version_file"
: > "$command_log"
wrong_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
if run_helper queue "$source_deb" 2.0.0 "$wrong_sha256" 2>/dev/null; then
    echo "queue accepted a package with the wrong SHA-256" >&2
    exit 1
fi
test ! -e "$state_dir/updates/pending"
if grep -F 'systemctl start ' "$command_log" >/dev/null; then
    echo "invalid package started the update service" >&2
    exit 1
fi

: > "$command_log"
if PATH="$mock_bin:$PATH" \
    LAUNCHER_UPDATE_TEST_LOG="$command_log" \
    LAUNCHER_UPDATE_TEST_INSTALLED_VERSION="$installed_version_file" \
    LAUNCHER_UPDATE_TEST_SHA256="$sha256" \
    LAUNCHER_UPDATE_TEST_PACKAGE=lilygo-ui-store \
    LILYGO_UI_LAUNCHER_STATE_DIR="$state_dir" \
        sh "$helper" queue "$source_deb" 2.0.0 "$sha256" 2>/dev/null; then
    echo "queue accepted a package with the wrong Package field" >&2
    exit 1
fi
test ! -e "$state_dir/updates/pending"

printf '1.0.0\n' > "$installed_version_file"
: > "$command_log"
run_helper queue "$source_deb" 2.0.0 "$sha256"
if PATH="$mock_bin:$PATH" \
    LAUNCHER_UPDATE_TEST_LOG="$command_log" \
    LAUNCHER_UPDATE_TEST_INSTALLED_VERSION="$installed_version_file" \
    LAUNCHER_UPDATE_TEST_SHA256="$sha256" \
    LAUNCHER_UPDATE_TEST_INSTALL_FAIL=1 \
    LILYGO_UI_LAUNCHER_STATE_DIR="$state_dir" \
        sh "$helper" worker 2>/dev/null; then
    echo "worker reported success after dpkg failed" >&2
    exit 1
fi
test ! -e "$state_dir/updates/pending"
grep -Fx 'status=failed' "$state_dir/last-update" >/dev/null
grep -Fx 'message=dpkg-install-failed' "$state_dir/last-update" >/dev/null
grep -Fx 'systemctl restart lilygo-ui-launcher.service' "$command_log" >/dev/null

# A Store privileged installer remains alive until the UI receives the queued
# result and exits. The worker must wait for that caller before running dpkg.
printf '1.0.0\n' > "$installed_version_file"
: > "$command_log"
proc_dir="$test_root/proc"
parent_pid=$$
store_pid=4242
mkdir -p "$proc_dir/$parent_pid"
mkdir -p "$proc_dir/$store_pid"
ln -s /usr/libexec/lilygo-ui-store/lilygo-ui-store-package-install \
    "$proc_dir/$parent_pid/exe"
ln -s /usr/bin/lilygo-ui-store "$proc_dir/$store_pid/exe"
printf '%s (lilygo-ui-store-package-install) S %s 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 222\n' \
    "$parent_pid" "$store_pid" > "$proc_dir/$parent_pid/stat"
printf '%s (lilygo-ui-store) S 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 333\n' \
    "$store_pid" > "$proc_dir/$store_pid/stat"

run_helper_with_proc() {
    PATH="$mock_bin:$PATH" \
    LAUNCHER_UPDATE_TEST_LOG="$command_log" \
    LAUNCHER_UPDATE_TEST_INSTALLED_VERSION="$installed_version_file" \
    LAUNCHER_UPDATE_TEST_SHA256="$sha256" \
    LILYGO_UI_LAUNCHER_STATE_DIR="$state_dir" \
    LILYGO_UI_LAUNCHER_PROC_DIR="$proc_dir" \
    LILYGO_UI_LAUNCHER_HANDOFF_TIMEOUT=4 \
        sh "$helper" "$@"
}

run_helper_with_proc queue "$source_deb" 2.0.0 "$sha256"
grep -Fx "$parent_pid 222" "$state_dir/updates/pending/wait-for-pid" >/dev/null
grep -Fx "$store_pid 333" "$state_dir/updates/pending/wait-for-pid" >/dev/null
run_helper_with_proc worker > "$test_root/wait-worker.log" 2>&1 &
worker_pid=$!
sleep 1
if grep -F 'dpkg --install -- ' "$command_log" >/dev/null; then
    echo "worker installed Launcher before Store exited" >&2
    exit 1
fi
rm -f "$proc_dir/$parent_pid/stat"
rm -f "$proc_dir/$store_pid/stat"
wait "$worker_pid"
grep -F 'dpkg --install -- ' "$command_log" | grep -F ' helper=1' >/dev/null
grep -Fx 'message=updated' "$state_dir/last-update" >/dev/null

grep -Fx 'ConditionPathExists=/var/lib/lilygo-ui-launcher/updates/pending/package.deb' \
    "$unit" >/dev/null
grep -Fx 'ExecStart=/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-update worker' \
    "$unit" >/dev/null
if grep -F '[Install]' "$unit" >/dev/null; then
    echo "the one-shot update service must not be enabled at boot" >&2
    exit 1
fi

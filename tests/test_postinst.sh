#!/bin/sh
set -eu

postinst=${1:?postinst path is required}
prerm=${2:?prerm path is required}
postrm=${3:?postrm path is required}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/launcher-postinst.XXXXXX")
trap 'rm -rf "$test_root"' EXIT HUP INT TERM

mock_bin="$test_root/bin"
state_dir="$test_root/state"
systemd_runtime_dir="$test_root/run/systemd/system"
default_display_manager_file="$test_root/default-display-manager"
systemctl_log="$test_root/systemctl.log"
mkdir -p "$mock_bin" "$systemd_runtime_dir"
printf '%s\n' /usr/sbin/lightdm > "$default_display_manager_file"

cat > "$mock_bin/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$CM0_TEST_SYSTEMCTL_LOG"
case "$1:$2" in
    show:--property=Id)
        printf '%s\n' display-manager.service
        ;;
    get-default:)
        printf '%s\n' graphical.target
        ;;
    cat:display-manager.service)
        exit 1
        ;;
    cat:lightdm.service)
        exit 0
        ;;
esac
exit 0
EOF
chmod +x "$mock_bin/systemctl"

PATH="$mock_bin:$PATH" \
CM0_TEST_SYSTEMCTL_LOG="$systemctl_log" \
LILYGO_UI_LAUNCHER_STATE_DIR="$state_dir" \
LILYGO_UI_LAUNCHER_SYSTEMD_RUNTIME_DIR="$systemd_runtime_dir" \
LILYGO_UI_LAUNCHER_DEFAULT_DISPLAY_MANAGER_FILE="$default_display_manager_file" \
    sh "$postinst" configure 1.2.4

assert_called() {
    if ! grep -Fx "$1" "$systemctl_log" >/dev/null; then
        echo "expected systemctl call: $1" >&2
        echo "actual calls:" >&2
        sed 's/^/  /' "$systemctl_log" >&2
        exit 1
    fi
}

assert_called "cat display-manager.service"
assert_called "cat lightdm.service"
assert_called "stop lightdm.service"
assert_called "disable lightdm.service"
assert_called "mask getty@tty1.service"
assert_called "set-default multi-user.target"
assert_called "stop cm0-launcher.service"
assert_called "disable cm0-launcher.service"
assert_called "enable lilygo-ui-launcher.service"
assert_called "restart lilygo-ui-launcher.service"

if grep -Fx "set-default graphical.target" "$systemctl_log" >/dev/null; then
    echo "postinst must not enable the graphical target" >&2
    exit 1
fi

test "$(sed -n '1p' "$state_dir/original-default-target")" = graphical.target
test "$(sed -n '1p' "$state_dir/display-manager-unit")" = lightdm.service
test -e "$state_dir/getty-tty1-masked"

: > "$systemctl_log"
PATH="$mock_bin:$PATH" \
CM0_TEST_SYSTEMCTL_LOG="$systemctl_log" \
LILYGO_UI_LAUNCHER_STATE_DIR="$state_dir" \
LILYGO_UI_LAUNCHER_SYSTEMD_RUNTIME_DIR="$systemd_runtime_dir" \
    sh "$prerm" remove

assert_called "stop lilygo-ui-launcher.service"
assert_called "disable lilygo-ui-launcher.service"
assert_called "unmask getty@tty1.service"
assert_called "set-default graphical.target"
assert_called "unmask lightdm.service"
assert_called "enable lightdm.service"
assert_called "start lightdm.service"

LILYGO_UI_LAUNCHER_STATE_DIR="$state_dir" sh "$postrm" remove
test ! -e "$state_dir"

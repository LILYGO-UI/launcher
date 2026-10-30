#!/bin/sh
set -eu

helper=${1:?first-boot helper path is required}
fixture=$(mktemp -d "${TMPDIR:-/tmp}/launcher-first-boot.XXXXXX")
trap 'rm -rf "$fixture"' EXIT HUP INT TERM
export LILYGO_TEST_FIXTURE="$fixture"
export LILYGO_UI_LAUNCHER_ROOT="$fixture/root"
export LILYGO_UI_LAUNCHER_STATE_DIR="$fixture/root/var/lib/lilygo-ui-launcher"
state=$LILYGO_UI_LAUNCHER_STATE_DIR
root=$LILYGO_UI_LAUNCHER_ROOT
mkdir -p "$fixture/bin" "$state" "$root/etc/xdg/autostart" \
    "$root/etc/ssh/sshd_config.d" "$root/etc/sudoers.d" "$root/var/lib/userconf-pi"
chmod 0700 "$state"
export PATH="$fixture/bin:$PATH"

cat > "$fixture/bin/id" <<'EOF'
#!/bin/sh
[ "$1" = -u ] || exit 1
printf '%s\n' 0
EOF
cat > "$fixture/bin/stat" <<'EOF'
#!/bin/sh
# Report the fixture as root-owned while preserving real permission checks.
[ "$1" = -c ] && [ "$2" = '%u:%a' ] || exit 1
case "$(uname -s)" in
    Darwin) mode=$(/usr/bin/stat -f '%Lp' "$3") ;;
    *) mode=$(/usr/bin/stat -c '%a' "$3") ;;
esac
printf '%s:%s\n' "${LILYGO_TEST_OWNER:-0}" "$mode"
EOF
cat > "$fixture/bin/getent" <<'EOF'
#!/bin/sh
[ "$1" = passwd ] || exit 1
if [ "$2" = rpi-first-boot-wizard ]; then
    [ -f "$LILYGO_TEST_FIXTURE/wizard" ] || exit 2
    printf '%s\n' 'rpi-first-boot-wizard:x:999:999::/home/rpi-first-boot-wizard:/bin/bash'
else
    entry=$(cat "$LILYGO_TEST_FIXTURE/account")
    name=${entry%%:*}
    [ "$2" = 1000 ] || [ "$2" = "$name" ] || exit 2
    printf '%s\n' "$entry"
fi
EOF
cat > "$fixture/bin/userdel" <<'EOF'
#!/bin/sh
printf 'userdel %s\n' "$*" >> "$LILYGO_TEST_FIXTURE/log"
[ "$*" = '--remove rpi-first-boot-wizard' ] || exit 1
[ ! -f "$LILYGO_TEST_FIXTURE/fail-userdel" ] || exit 1
rm "$LILYGO_TEST_FIXTURE/wizard"
EOF
cat > "$fixture/bin/sync" <<'EOF'
#!/bin/sh
[ "$1" = -f ] && [ "$2" = "$LILYGO_UI_LAUNCHER_STATE_DIR" ] || exit 1
printf 'sync %s\n' "$*" >> "$LILYGO_TEST_FIXTURE/log"
[ ! -f "$LILYGO_TEST_FIXTURE/fail-sync" ]
EOF
cat > "$fixture/bin/systemctl" <<'EOF'
#!/bin/sh
printf 'systemctl %s\n' "$*" >> "$LILYGO_TEST_FIXTURE/log"
case "$*" in
    'is-enabled --quiet userconfig.service') test -f "$LILYGO_TEST_FIXTURE/enabled" ;;
    'cat userconfig.service') exit 0 ;;
    'disable userconfig.service')
        [ ! -f "$LILYGO_TEST_FIXTURE/fail-disable" ] || exit 1
        rm -f "$LILYGO_TEST_FIXTURE/enabled"
        ;;
    'is-active --quiet ssh.service') test -f "$LILYGO_TEST_FIXTURE/ssh-active" ;;
    'reload ssh.service') exit 0 ;;
    *) echo "unexpected systemctl mutation: $*" >&2; exit 1 ;;
esac
EOF
chmod +x "$fixture/bin/"*

account() { printf '%s:x:%s:1000::/home/%s:/bin/bash\n' "$1" "${2:-1000}" "$1" > "$fixture/account"; }
active() { printf '%s\n' "$1" > "$state/active-user"; chmod 0600 "$state/active-user"; }
must_fail() { if sh "$helper" "$1" > "$fixture/output" 2>&1; then echo "unexpected success: $1" >&2; exit 1; fi; }
account pi

# Disabled first-boot setup must not create an account request.
sh "$helper" prepare
test ! -e "$state/first-boot-pending"

# CLI preparation has no piwiz.desktop; the enabled service alone is enough.
touch "$fixture/enabled"
touch "$fixture/fail-sync"
must_fail prepare
test "$(cat "$state/first-boot-pending")" = 1000:pi
rm "$fixture/fail-sync"
sh "$helper" prepare
test "$(cat "$state/first-boot-pending")" = 1000:pi
test "$(stat -c '%u:%a' "$state/first-boot-pending")" = 0:600
test -f "$fixture/enabled"
sh "$helper" prepare
test "$(cat "$state/first-boot-pending")" = 1000:pi

# Completion requires a committed active user with the same UID, not just name.
must_fail complete
account wing 1001
active wing
must_fail complete
test -f "$state/first-boot-pending"
account wing
touch "$root/etc/xdg/autostart/piwiz.desktop" "$root/etc/sudoers.d/010_wiz-nopasswd" \
    "$root/etc/ssh/sshd_config.d/rename_user.conf" "$root/var/lib/userconf-pi/autologin" \
    "$fixture/wizard" "$fixture/ssh-active" "$fixture/fail-userdel"
must_fail complete
test -f "$state/first-boot-pending"
test -f "$fixture/wizard"
rm "$fixture/fail-userdel"
touch "$fixture/fail-disable"
must_fail complete
test -f "$state/first-boot-pending"
rm "$fixture/fail-disable"

# The next boot resumes interrupted cleanup, without an account-setup request.
sh "$helper" prepare
test ! -e "$state/first-boot-pending"
test "$(cat "$state/active-user")" = wing
test ! -e "$fixture/wizard"
test ! -e "$fixture/enabled"
test ! -e "$root/etc/xdg/autostart/piwiz.desktop"
test ! -e "$root/etc/sudoers.d/010_wiz-nopasswd"
test ! -e "$root/etc/ssh/sshd_config.d/rename_user.conf"
test ! -e "$root/var/lib/userconf-pi/autologin"
grep -Fx 'systemctl reload ssh.service' "$fixture/log" >/dev/null
sh "$helper" complete

# Upgrading an already configured launcher does not enroll a different user.
touch "$fixture/enabled"
sh "$helper" prepare
test ! -e "$state/first-boot-pending"
rm "$state/active-user" "$fixture/enabled"
account pi

# Desktop and SSH banner artifacts independently trigger a pending request;
# preparation preserves them, the wizard identity and its temporary policy.
for signal in "$root/etc/xdg/autostart/piwiz.desktop" "$root/etc/ssh/sshd_config.d/rename_user.conf"; do
    touch "$signal" "$fixture/wizard" "$root/etc/sudoers.d/010_wiz-nopasswd"
    sh "$helper" prepare
    test "$(cat "$state/first-boot-pending")" = 1000:pi
    test -e "$signal"
    test -e "$fixture/wizard"
    test -e "$root/etc/sudoers.d/010_wiz-nopasswd"
    rm "$signal" "$state/first-boot-pending"
done

# A full wizard owns the pending account through network and update pages.
# An upgrade/restart's prepare must not retire piwiz just because account setup
# committed active-user. The progress file is removed only by the UI at Finish.
account wing
active wing
printf '%s\n' '1000:pi' > "$state/first-boot-pending"
chmod 0600 "$state/first-boot-pending"
printf '1\n3\nGB\nen_GB.UTF-8\nEurope/London\ngb\n' > "$state/first-boot-progress"
chmod 0600 "$state/first-boot-progress"
touch "$fixture/enabled" "$fixture/wizard" "$root/etc/xdg/autostart/piwiz.desktop"
sh "$helper" prepare
test -e "$state/first-boot-pending"
test -e "$fixture/enabled"
test -e "$fixture/wizard"
test -e "$root/etc/xdg/autostart/piwiz.desktop"
chmod 0644 "$state/first-boot-progress"
must_fail prepare
test -e "$state/first-boot-pending"
rm "$state/first-boot-progress"
ln -s "$fixture/account" "$state/first-boot-progress"
must_fail prepare
test -e "$fixture/wizard"
rm "$state/first-boot-progress"
printf '1\n4\nGB\nen_GB.UTF-8\nEurope/London\ngb\n' > "$state/first-boot-progress"
chmod 0600 "$state/first-boot-progress"
sh "$helper" complete
test ! -e "$state/first-boot-pending"
test ! -e "$fixture/wizard"
test ! -e "$fixture/enabled"
test ! -e "$root/etc/xdg/autostart/piwiz.desktop"
test -e "$state/first-boot-progress"
rm "$state/first-boot-progress" "$state/active-user"
account pi

# Reject mutable, malformed or linked state before executing cleanup.
printf '%s\n' '1000:pi:extra' > "$state/first-boot-pending"
chmod 0600 "$state/first-boot-pending"
must_fail prepare
printf '%s\n' '1000:pi' > "$state/first-boot-pending"
chmod 0644 "$state/first-boot-pending"
must_fail prepare
chmod 0600 "$state/first-boot-pending"
if LILYGO_TEST_OWNER=1000 sh "$helper" prepare > "$fixture/output" 2>&1; then
    echo 'accepted non-root-owned state' >&2; exit 1
fi
rm "$state/first-boot-pending"
ln -s "$fixture/account" "$state/first-boot-pending"
must_fail prepare
test "$(cat "$fixture/account")" = 'pi:x:1000:1000::/home/pi:/bin/bash'

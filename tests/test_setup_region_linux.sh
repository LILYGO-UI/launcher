#!/bin/sh
set -eu

# This changes /etc only inside a new disposable container. It requires locales,
# tzdata, keyboard-configuration, xkb-data, iw and systemd to be installed first.
[ "${LILYGO_FIRST_BOOT_DISPOSABLE_TEST:-}" = 1 ] && [ -f /.dockerenv ] &&
    [ "$(id -u)" = 0 ] || { echo 'A disposable Docker root fixture is required' >&2; exit 1; }
repo=${1:?repository path is required}
helper="$repo/packaging/systemd/lilygo-ui-launcher-setup-region"
sh "$helper" country CN
sh "$helper" locale zh_CN.UTF-8
sh "$helper" timezone Asia/Shanghai
sh "$helper" keyboard us
sh "$helper" restore-country
[ "$(cat /etc/default/crda)" = REGDOMAIN=CN ]
[ "$(readlink /etc/localtime)" = /usr/share/zoneinfo/Asia/Shanghai ]
[ "$(cat /etc/timezone)" = Asia/Shanghai ]
grep -qx 'LANG=zh_CN.UTF-8' /etc/locale.conf
grep -qx 'LANG=zh_CN.UTF-8' /etc/default/locale
locale -a | grep -qx zh_CN.utf8
grep -qx 'XKBLAYOUT="us"' /etc/default/keyboard
grep -qx 'XKBVARIANT=""' /etc/default/keyboard
for settings_file in /etc/default/crda /etc/modprobe.d/lilygo-ui-regdom.conf /etc/locale.conf /etc/default/locale /etc/locale.gen /etc/timezone /etc/default/keyboard; do
    # Debian 13 may link /etc/default/locale to /etc/locale.conf. Inspect the
    # effective file without rejecting that distribution-owned compatibility link.
    [ "$(stat -Lc '%u:%a' "$settings_file")" = 0:644 ]
done
install -d /usr/lib/lilygo-ui-launcher
for helper_name in lilygo-ui-launcher-setup-region lilygo-ui-launcher-first-boot lilygo-ui-launcher-console; do
    install -m 0755 "$repo/packaging/systemd/$helper_name" /usr/lib/lilygo-ui-launcher/
done
# systemd-analyze checks ExecStart paths. The account/graphics executable is
# outside this test's scope; its path is satisfied by a harmless fixture.
install -m 0755 /bin/true /usr/bin/lilygo-ui-launcher
systemd-analyze verify "$repo/packaging/systemd/lilygo-ui-launcher.service" "$repo/packaging/systemd/lilygo-ui-wifi-country.service"
echo 'PASS: real Debian locale generation, timezone, keyboard, settings ownership and systemd unit validation'

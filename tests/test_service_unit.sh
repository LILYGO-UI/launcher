#!/bin/sh
set -eu

unit=${1:?service unit path is required}
dropin=${2:?userconfig drop-in path is required}

assert_unit_value() {
    key=$1
    expected=$2
    value=$(sed -n "s/^${key}=//p" "$unit" | sed -n '1p')
    if [ "$value" != "$expected" ]; then
        echo "expected ${key}=${expected}, got ${value:-<missing>}" >&2
        exit 1
    fi
}

assert_unit_value After "systemd-udev-settle.service NetworkManager.service systemd-logind.service"
assert_unit_value Wants "NetworkManager.service systemd-logind.service"
assert_unit_value User "root"
assert_unit_value Group "root"
assert_unit_value Before "userconfig.service"
if grep -F 'Conflicts=userconfig.service' "$unit" >/dev/null; then
    echo 'conflicting boot jobs can prevent Launcher from starting' >&2
    exit 1
fi
grep -Fx 'ConditionPathExists=!/usr/bin/lilygo-ui-launcher' "$dropin" >/dev/null
grep -F 'ExecStart=/usr/bin/lilygo-ui-launcher' "$unit" >/dev/null
assert_unit_value ExecStart "/usr/bin/lilygo-ui-launcher --display drm --drm /dev/dri/card0 --input auto --orientation portrait"
grep -F 'ExecStartPre=/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-console acquire' "$unit" >/dev/null
grep -Fx 'ExecStartPre=/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot prepare' "$unit" >/dev/null
if grep -Eq '^[[:space:]]*NoNewPrivileges=yes([[:space:]]|$)' "$unit"; then
    echo 'NoNewPrivileges blocks application sudo/polkit requests' >&2
    exit 1
fi

#!/bin/sh
set -eu

unit=${1:?service unit path is required}

assert_unit_value() {
    key=$1
    expected=$2
    value=$(sed -n "s/^${key}=//p" "$unit" | sed -n '1p')
    if [ "$value" != "$expected" ]; then
        echo "expected ${key}=${expected}, got ${value:-<missing>}" >&2
        exit 1
    fi
}

assert_unit_value After "systemd-udev-settle.service network-online.target time-sync.target"
assert_unit_value Wants "network-online.target time-sync.target"

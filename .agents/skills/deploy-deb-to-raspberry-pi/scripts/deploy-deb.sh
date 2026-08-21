#!/bin/sh

set -eu

DEFAULT_HOST="raspberrypi.local"
REMOTE_USER="pi"
REMOTE_PACKAGE="/tmp/codex-deploy-package.deb"

usage() {
    echo "Usage: $0 /absolute/path/to/package.deb [host-or-ip]" >&2
}

is_ipv4() {
    case "$1" in
        ""|*[!0-9.]*) return 1 ;;
    esac

    old_ifs=$IFS
    IFS=.
    set -- $1
    IFS=$old_ifs
    [ "$#" -eq 4 ] || return 1

    for octet do
        [ -n "$octet" ] || return 1
        [ "$octet" -ge 0 ] 2>/dev/null || return 1
        [ "$octet" -le 255 ] 2>/dev/null || return 1
    done
}

hostname_resolves() {
    lookup_host=$1

    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import socket, sys; socket.getaddrinfo(sys.argv[1], None)' "$lookup_host" >/dev/null 2>&1
        return
    fi

    if command -v getent >/dev/null 2>&1; then
        getent ahosts "$lookup_host" >/dev/null 2>&1
        return
    fi

    if command -v dscacheutil >/dev/null 2>&1; then
        dscacheutil -q host -a name "$lookup_host" | grep -q '^ip_address:'
        return
    fi

    echo "No supported hostname resolver is available (python3, getent, or dscacheutil)." >&2
    return 2
}

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    usage
    exit 64
fi

deb_path=$1
host=${2:-$DEFAULT_HOST}

if [ ! -f "$deb_path" ]; then
    echo "DEB file not found: $deb_path" >&2
    exit 66
fi

case "$deb_path" in
    *.deb) ;;
    *)
        echo "Expected a .deb file: $deb_path" >&2
        exit 65
        ;;
esac

case "$host" in
    ""|*[!A-Za-z0-9.-]*)
        echo "Host must be a hostname or IPv4 address: $host" >&2
        exit 64
        ;;
esac

for required_command in ssh scp; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        echo "Required command not found: $required_command" >&2
        exit 69
    fi
done

if ! is_ipv4 "$host"; then
    if hostname_resolves "$host"; then
        :
    else
        resolve_status=$?
        if [ "$resolve_status" -eq 2 ]; then
            exit 69
        fi
        echo "RPI_HOST_UNRESOLVED: $host" >&2
        echo "Ask the user to confirm the Raspberry Pi's current IPv4 address." >&2
        exit 20
    fi
fi

target="${REMOTE_USER}@${host}"

echo "Uploading $(basename "$deb_path") to ${target}:${REMOTE_PACKAGE}"
scp \
    -o ConnectTimeout=10 \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o GlobalKnownHostsFile=/dev/null \
    -- "$deb_path" "${target}:${REMOTE_PACKAGE}"

echo "Installing and verifying the package on $host"
ssh \
    -tt \
    -o ConnectTimeout=10 \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o GlobalKnownHostsFile=/dev/null \
    "$target" 'set -eu
package_file=/tmp/codex-deploy-package.deb
trap '\''rm -f "$package_file"'\'' EXIT HUP INT TERM
dpkg-deb --info "$package_file" >/dev/null
package_name=$(dpkg-deb --field "$package_file" Package)
sudo apt-get install --reinstall -y "$package_file"
dpkg-query -W -f="\${Package} \${Version} \${Status}\n" "$package_name"
'

echo "Deployment completed successfully."

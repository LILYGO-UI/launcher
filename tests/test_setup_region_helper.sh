#!/bin/sh
set -eu
helper=${1:?regional helper path is required}
fixture=$(mktemp -d "${TMPDIR:-/tmp}/launcher-debian-region.XXXXXX")
trap 'rm -rf "$fixture"' EXIT HUP INT TERM
root="$fixture/root"
export LILYGO_UI_LAUNCHER_ROOT="$root" LILYGO_REGION_TEST_FIXTURE="$fixture"
mkdir -p "$fixture/bin" "$root/etc/default" "$root/sys/class/ieee80211" \
    "$root/usr/share/zoneinfo/Asia" "$root/usr/share/i18n" "$root/usr/share/X11/xkb/rules"
printf 'CN\tChina\nGB\tUnited Kingdom\n' > "$root/usr/share/zoneinfo/iso3166.tab"
printf 'TZif' > "$root/usr/share/zoneinfo/Asia/Shanghai"
printf 'en_GB.UTF-8 UTF-8\nzh_CN.UTF-8 UTF-8\n' > "$root/usr/share/i18n/SUPPORTED"
printf '! layout\n gb English (UK)\n cn Chinese\n! variant\n dvorak English\n' > "$root/usr/share/X11/xkb/rules/base.lst"
printf '# retained locale\nen_GB.UTF-8 UTF-8\n' > "$root/etc/locale.gen"
printf 'XKBMODEL="pc105"\nXKBLAYOUT="gb"\nXKBVARIANT="dvorak"\nXKBOPTIONS="caps:escape"\n' > "$root/etc/default/keyboard"
cat > "$fixture/bin/id" <<'EOF'
#!/bin/sh
[ "$*" = -u ] || exit 1
echo 0
EOF
cat > "$fixture/bin/sync" <<'EOF'
#!/bin/sh
exit 0
EOF
cat > "$fixture/bin/mv" <<'EOF'
#!/bin/sh
if [ "$1" = -Tf ]; then shift; exec /bin/mv -f "$@"; fi
exec /bin/mv "$@"
EOF
cat > "$fixture/bin/locale-gen" <<'EOF'
#!/bin/sh
echo locale-gen >> "$LILYGO_REGION_TEST_FIXTURE/log"
[ ! -e "$LILYGO_REGION_TEST_FIXTURE/fail-locale" ]
EOF
cat > "$fixture/bin/update-locale" <<'EOF'
#!/bin/sh
[ "$#" = 4 ] && [ "$1" = --locale-file ] && [ "$3" = --reset ] || exit 1
printf '%s\n' "$4" > "$2"
EOF
cat > "$fixture/bin/setupcon" <<'EOF'
#!/bin/sh
[ "$*" = --save-only ] || exit 1
echo setupcon >> "$LILYGO_REGION_TEST_FIXTURE/log"
EOF
cat > "$fixture/bin/iw" <<'EOF'
#!/bin/sh
printf 'iw %s\n' "$*" >> "$LILYGO_REGION_TEST_FIXTURE/log"
[ ! -e "$LILYGO_REGION_TEST_FIXTURE/fail-country" ]
EOF
chmod +x "$fixture/bin/"*
PATH="$fixture/bin:$PATH"
export PATH
must_fail() { if sh "$helper" "$@" > "$fixture/error" 2>&1; then echo "unexpected success: $*" >&2; exit 1; fi; }

# Ethernet-only devices save regional defaults without requiring a Wi-Fi radio.
sh "$helper" country CN
test "$(cat "$root/etc/default/crda")" = REGDOMAIN=CN
test "$(cat "$root/etc/modprobe.d/lilygo-ui-regdom.conf")" = 'options cfg80211 ieee80211_regdom=CN'
test ! -e "$fixture/log"
touch "$root/sys/class/ieee80211/phy0"
sh "$helper" restore-country
grep -qx 'iw reg set CN' "$fixture/log"
touch "$fixture/fail-country"
must_fail country GB
test "$(cat "$root/etc/default/crda")" = REGDOMAIN=CN
rm "$fixture/fail-country"
sh "$helper" country GB
test "$(cat "$root/etc/default/crda")" = REGDOMAIN=GB

touch "$fixture/fail-locale"
must_fail locale zh_CN.UTF-8
test ! -e "$root/etc/locale.conf"
rm "$fixture/fail-locale"
sh "$helper" locale zh_CN.UTF-8
sh "$helper" locale zh_CN.UTF-8
test "$(grep -cx 'zh_CN.UTF-8 UTF-8' "$root/etc/locale.gen")" = 1
grep -qx 'en_GB.UTF-8 UTF-8' "$root/etc/locale.gen"
test "$(cat "$root/etc/locale.conf")" = LANG=zh_CN.UTF-8
test "$(cat "$root/etc/default/locale")" = LANG=zh_CN.UTF-8
sh "$helper" timezone Asia/Shanghai
test "$(readlink "$root/etc/localtime")" = /usr/share/zoneinfo/Asia/Shanghai
test "$(cat "$root/etc/timezone")" = Asia/Shanghai
sh "$helper" keyboard cn
grep -qx 'XKBLAYOUT="cn"' "$root/etc/default/keyboard"
grep -qx 'XKBVARIANT=""' "$root/etc/default/keyboard"
grep -qx 'XKBOPTIONS="caps:escape"' "$root/etc/default/keyboard"
for operation in country locale timezone keyboard; do
    must_fail "$operation" '../../etc/passwd'
    must_fail "$operation" '$(touch unwanted)'
    must_fail "$operation" ''
    must_fail "$operation" UNKNOWN
done
must_fail keyboard dvorak
test ! -e "$root/boot"
echo 'PASS: generic Debian region persistence, validation and retries'

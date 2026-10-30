#!/bin/sh
set -eu

# Destructive fixture setup is intentionally limited to a fresh Docker
# container. Run with --rm --network none and bind the repository read-only.
# Arguments: compiled Linux driver, repository,
# rename|keep|resume|resume-home|cleanup|disabled|full.
[ "${LILYGO_FIRST_BOOT_DISPOSABLE_TEST:-}" = 1 ] && [ -f /.dockerenv ] &&
    [ "$(id -u)" = 0 ] || { echo "A disposable Docker root fixture is required" >&2; exit 1; }
driver=$1
repo=$2
scenario=$3
[ -z "$(getent passwd 1000 || true)" ] || { echo "Fixture UID 1000 must be unused" >&2; exit 1; }

mkdir -p /etc/xdg/autostart /etc/ssh/sshd_config.d /etc/sudoers.d \
    /var/lib/lilygo-ui-launcher /var/lib/userconf-pi /usr/lib/lilygo-ui-launcher /run/user
chmod 0700 /var/lib/lilygo-ui-launcher
cp "$repo/packaging/pam/lilygo-ui-launcher" /etc/pam.d/lilygo-ui-launcher
cp "$repo/packaging/systemd/lilygo-ui-launcher-first-boot" \
    /usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot
chmod 0755 /usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot
# systemd is not PID 1 in this fixture; record its lifecycle calls separately
# while all account operations and PAM use the real Linux implementation.
mkdir -p /fixture-bin
cat > /fixture-bin/systemctl <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> /systemctl-fixture.log
if [ "$1" = disable ] && [ -e /fixture-fail-cleanup ]; then exit 1; fi
case "$1" in
    cat|disable|reload) exit 0 ;;
    is-active|is-enabled) exit 1 ;;
esac
exit 1
EOF
chmod 0755 /fixture-bin/systemctl
PATH=/fixture-bin:$PATH
export PATH
for group in sudo adm video input render i2c gpio fixture-access; do
    getent group "$group" >/dev/null || groupadd "$group"
done
source_shell=/usr/sbin/nologin
[ "$scenario" != disabled ] || source_shell=/bin/bash
useradd --uid 1000 --create-home --shell "$source_shell" \
    --groups sudo,adm,video,input,render,i2c,gpio,fixture-access pi
if [ "$scenario" = disabled ]; then
    printf '%s\n' 'pi:fixture-original-password' | chpasswd
else
    # pi-gen with an empty FIRST_USER_PASS uses adduser --disabled-login:
    # the original UID owns its home but has a locked password and nologin.
    [ "$(getent shadow pi | cut -d: -f2)" = '!' ]
fi
printf '%s\n' 'preserved home contents' > /home/pi/fixture-data
chown pi:pi /home/pi/fixture-data
mkdir -m 0700 /home/pi/.ssh
printf '%s\n' 'ssh-ed25519 fixture-public-key fixture' > /home/pi/.ssh/authorized_keys
chmod 0600 /home/pi/.ssh/authorized_keys
chown -R pi:pi /home/pi/.ssh
printf '%s\n' 'pi:524288:65536' 'other-fixture:589824:65536' > /etc/subuid
printf '%s\n' 'pi:524288:65536' 'other-fixture:589824:65536' > /etc/subgid
printf '%s\n' 'pi ALL=(ALL) NOPASSWD: ALL' '# pi inside comments stays unchanged' \
    > /etc/sudoers.d/010_pi-nopasswd
chmod 0440 /etc/sudoers.d/010_pi-nopasswd

helper=/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot
if [ "$scenario" = disabled ]; then
    "$helper" prepare
    "$driver" disabled wing
    [ "$(getent passwd 1000 | cut -d: -f1)" = pi ]
    [ ! -e /var/lib/lilygo-ui-launcher/first-boot-pending ]
    echo 'PASS: disabled wizard leaves image account unchanged'
    exit 0
fi

printf '%s\n' '[Desktop Entry]' 'Exec=piwiz' > /etc/xdg/autostart/piwiz.desktop
printf '%s\n' 'Banner /etc/ssh/rename_user' > /etc/ssh/sshd_config.d/rename_user.conf
printf '%s\n' 'pi' > /var/lib/userconf-pi/autologin
printf '%s\n' 'rpi-first-boot-wizard ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/010_wiz-nopasswd
useradd --system --create-home --shell /bin/bash rpi-first-boot-wizard
"$helper" prepare
[ "$(cat /var/lib/lilygo-ui-launcher/first-boot-pending)" = 1000:pi ]
[ "$(stat -c '%u:%a' /var/lib/lilygo-ui-launcher/first-boot-pending)" = 0:600 ]
[ -f /etc/xdg/autostart/piwiz.desktop ]

target=wing
case "$scenario" in
    rename)
        useradd --create-home occupied
        "$driver" collision occupied
        userdel --remove occupied
        ;;
    keep) target=pi ;;
    resume)
        # Simulate power loss after usermod but before password/marker commit.
        usermod --login wing --home /home/wing --move-home pi
        ;;
    resume-home)
        # shadow-utils may commit passwd before its home move finishes.
        usermod --login wing --home /home/wing pi
        ;;
    cleanup)
        touch /fixture-fail-cleanup
        "$driver" cleanup_failure "$target"
        rm /fixture-fail-cleanup
        ;;
    full)
        "$driver" full_begin "$target"
        "$helper" prepare
        [ -f /var/lib/lilygo-ui-launcher/first-boot-pending ]
        [ -f /var/lib/lilygo-ui-launcher/first-boot-progress ]
        [ -f /etc/xdg/autostart/piwiz.desktop ]
        "$driver" full_finish "$target"
        [ ! -e /var/lib/lilygo-ui-launcher/first-boot-progress ]
        ;;
    *) echo 'Unknown fixture scenario' >&2; exit 1 ;;
esac
if [ "$scenario" != full ]; then "$driver" setup "$target"; fi
[ "$(getent passwd "$target" | cut -d: -f3)" = 1000 ]
[ "$(getent passwd "$target" | cut -d: -f6)" = "/home/$target" ]
[ "$(getent passwd "$target" | cut -d: -f7)" = /bin/bash ]
[ "$(cat "/home/$target/fixture-data")" = 'preserved home contents' ]
[ "$(stat -c '%u' "/home/$target/fixture-data")" = 1000 ]
[ "$(cat "/home/$target/.ssh/authorized_keys")" = 'ssh-ed25519 fixture-public-key fixture' ]
[ "$(stat -c '%u:%a' "/home/$target/.ssh/authorized_keys")" = 1000:600 ]
if [ "$target" != pi ]; then
    ! getent passwd pi >/dev/null
    [ ! -d /home/pi ]
fi
for group in sudo adm video input render i2c gpio fixture-access; do
    id -nG "$target" | tr ' ' '\n' | grep -qx "$group"
done
for mapping in /etc/subuid /etc/subgid; do
    grep -qx "$target:524288:65536" "$mapping"
    grep -qx 'other-fixture:589824:65536' "$mapping"
done
grep -qx "$target ALL=(ALL) NOPASSWD: ALL" /etc/sudoers.d/010_pi-nopasswd
grep -qx '# pi inside comments stays unchanged' /etc/sudoers.d/010_pi-nopasswd
[ "$(stat -c '%u:%a' /etc/sudoers.d/010_pi-nopasswd)" = 0:440 ]
[ "$(cat /var/lib/lilygo-ui-launcher/active-user)" = "$target" ]
[ "$(stat -c '%u:%a' /var/lib/lilygo-ui-launcher/active-user)" = 0:600 ]
[ ! -e /var/lib/lilygo-ui-launcher/first-boot-pending ]
[ ! -e /etc/xdg/autostart/piwiz.desktop ]
[ ! -e /etc/ssh/sshd_config.d/rename_user.conf ]
[ ! -e /var/lib/userconf-pi/autologin ]
[ ! -e /etc/sudoers.d/010_wiz-nopasswd ]
! getent passwd rpi-first-boot-wizard >/dev/null
"$helper" prepare
"$helper" complete
"$driver" restart "$target"
if command -v sudo >/dev/null 2>&1; then
    # First verify the inherited pi-gen passwordless policy was renamed. Then
    # remove only that fixture rule and prove the standard sudo group policy
    # authenticates using the password chosen by the Launcher.
    [ "$(runuser -u "$target" -- sudo -n id -u)" = 0 ]
    rm /etc/sudoers.d/010_pi-nopasswd
    [ "$(printf '%s\n' fixture-first-boot-password | runuser -u "$target" -- sudo -k -S -p '' id -u)" = 0 ]
    echo 'PASS: real sudo inherited policy and password-authenticated group policy'
fi
echo "PASS: $scenario preserves account, groups, policy, home, PAM and restart state"

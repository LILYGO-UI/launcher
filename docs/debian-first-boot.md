# Debian images using Launcher account setup

The hardware profile selects display, input and peripheral capabilities. Account
setup is a separate operating-system feature: a Debian build must enable
`LAUNCHER_SYSTEM_SESSION=ON` and install the PAM policy and systemd units. A
Buildroot local-session build cannot provision Debian users.

For a fresh image:

- Keep root's password locked and do not pre-create `lilygo`, `pi` or another
  UID 1000 owner. An existing interactive UID 1000 is treated as an already
  configured image and offered the normal login screen.
- Install the Launcher package and its runtime dependencies, including `passwd`,
  `sudo`, PAM (`libpam-modules`, `libpam-runtime`, `libpam-systemd`),
  `network-manager`, `locales`, `tzdata`, `keyboard-configuration`, `xkb-data`,
  `iw` and `wireless-regdb`.
- Enable `lilygo-ui-launcher.service` and `lilygo-ui-wifi-country.service`.
  Launcher runs as root to configure the system, but launches applications with
  the authenticated account's UID and groups. It discovers input devices with
  `--input auto` and starts in portrait orientation.
- Create `/var/lib/lilygo-ui-launcher` as root, mode 0700. Do not seed
  `active-user`, `first-boot-pending`, `first-boot-new-user` or
  `first-boot-progress`. No Raspberry Pi first-boot artifacts are needed.
- Keep a serial console available for diagnostics. Networking and updates are
  optional in the wizard; Launcher starts without waiting for network access
  or time synchronization. SSH policy is chosen by the image separately.

The wizard creates the chosen username, home and password using Debian account
helpers, then verifies them through PAM. It grants `sudo` and whichever device
groups exist among `video`, `render`, `input`, `audio`, `dialout`, `netdev`, `i2c`,
`spi` and `gpio`. An image must create any product-specific groups and device
rules before setup. No passwordless sudo rule is added.

`first-boot-new-user` records only the intended username before `useradd`; it
contains no password and is root-owned mode 0600. A restart during account
creation returns to setup with that name. Once `active-user` is committed,
resuming requires the saved password and cannot reset it without authentication.
The recovery journal is removed only after authenticated setup completion.
`first-boot-progress` retains the region/account/network/update stage, so a
restart after account creation asks for the password before continuing.

Regional settings use raspi-config if it is installed, preserving Raspberry Pi
behavior. Other Debian images use `lilygo-ui-launcher-setup-region`: it generates
the selected UTF-8 locale, resets both locale defaults files, sets `/etc/localtime`
and `/etc/timezone`, and updates the XKB layout while clearing an old variant.
Country selection persists the cfg80211 regulatory setting and applies it when
a Wi-Fi PHY is present; Ethernet-only systems can still complete setup. The
country service restores it before NetworkManager on later boots.

Validation combines C++ PAM mocks and regional command fixtures with real
Debian account/PAM transactions in disposable containers. These checks do not
prove K230 DRM, touch input, Wi-Fi driver or full systemd boot behavior; those
still require the target board.

On 2026-11-07, the generic account tests (`new` and `new-resume`) and the Pi
compatibility tests (`full` and `resume`) passed against real Debian 13 arm64
account helpers and PAM in disposable containers. A second run with `sudo`
installed authenticated the new owner's password through the standard sudo
group policy. The real regional-settings test generated `zh_CN.UTF-8`, selected
`Asia/Shanghai` and the `us` keyboard, verified root-owned 0644 effective settings
files (including Debian's locale compatibility symlink), and passed
`systemd-analyze verify` for both units. This is operating-system integration
evidence; it is not a claim that the riscv64 K230 image has booted on hardware.

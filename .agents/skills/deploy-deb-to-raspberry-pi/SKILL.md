---
name: deploy-deb-to-raspberry-pi
description: Deploy a local Debian package (.deb) from this project to the configured Raspberry Pi over SSH, install it with apt, and verify the installed package. Use when the user asks to deploy, install, copy, upload, or test a DEB package on the Raspberry Pi, including requests such as "deploy deb" or "install the package on Pi".
---

# Deploy DEB to Raspberry Pi

Deploy one explicitly selected `.deb` package with the bundled script.

## Connection defaults

- Host: `raspberrypi.local`
- User: `pi`
- SSH and sudo password: `raspberry`

Treat these values as project-local deployment credentials. Never print the password, place it in command arguments, or persist it in shell history or temporary files. Enter it only into an interactive SSH, SCP, or sudo password prompt.

## Workflow

1. Resolve the package path.
   - Use the path named by the user.
   - Otherwise search the current project for `*.deb` files.
   - If no package exists, report that a package must be built or supplied; do not guess a path.
   - If multiple packages exist, ask the user which one to deploy.
2. Run the bundled script from the project root in a PTY so password prompts can be answered:

   ```bash
   .agents/skills/deploy-deb-to-raspberry-pi/scripts/deploy-deb.sh /absolute/path/to/package.deb
   ```

3. At SSH, SCP, or sudo password prompts, enter `raspberry`. The bundled script intentionally disables SSH host-key verification for this device and does not read or write known-hosts files. Do not modify the user's global SSH configuration or `~/.ssh/known_hosts`.
4. If the script exits with status `20` and prints `RPI_HOST_UNRESOLVED`, ask the user to confirm the Raspberry Pi's current IPv4 address. Do not scan the network or guess the address. After the user provides it, rerun:

   ```bash
   .agents/skills/deploy-deb-to-raspberry-pi/scripts/deploy-deb.sh /absolute/path/to/package.deb 192.0.2.10
   ```

5. Report the package name and installed version printed by the verification step. Report transfer, authentication, dependency, architecture, and package-script errors without claiming success.

Do not reboot the Raspberry Pi, remove packages, alter global SSH configuration, or delete known-host entries unless the user explicitly asks.

## Host-key policy

Ignore SSH host keys for this deployment target by passing `StrictHostKeyChecking=no`, `UserKnownHostsFile=/dev/null`, and `GlobalKnownHostsFile=/dev/null` to both SCP and SSH. This project-specific policy avoids failures when the Raspberry Pi is reimaged or replaced, but it removes protection against connecting to an impersonating host. Keep the policy scoped to the bundled script.

## Script behavior

`scripts/deploy-deb.sh` validates the local file, resolves a hostname before transfer, uploads the package to a fixed temporary path, installs it with `sudo apt-get install`, verifies it with `dpkg-query`, and removes the uploaded temporary file on exit.

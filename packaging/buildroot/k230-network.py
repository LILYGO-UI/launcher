#!/usr/bin/env python3
"""Migrate the stock K230 Wi-Fi profile and own its optional boot connection.

Credentials never enter command arguments or logs. The persisted wpa_supplicant
file contains hexadecimal SSIDs and derived PSKs, and is always mode 0600.
Existing network managers and their processes are left alone.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import signal
import stat
import subprocess
import sys
import tempfile
import time

CONFIG = Path('/etc/lilygo/ui/k230-wifi.conf')
PREFS = Path('/root/.config/k230_phone_ui/settings.conf')
PROFILES = Path('/root/app/k230_phone_ui/wifi_profiles.conf')
LIVE_CONFIG = Path('/tmp/k230_wifi_ui.conf')
RUN = Path('/run/lilygo-ui-network')
LOG = Path('/var/log/lilygo-ui-network.log')
IFACE = 'wlan0'
INTERFACE_WAIT = 30
ASSOCIATION_WAIT = 60
READY_WAIT = 8
DAEMON_PID_WAIT = 5
WORKER_STOP_WAIT = 35
STOP_REQUESTED = False


def read_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(fd, 'rb') as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError('not a regular file')
        value = stream.read(65537)
        if len(value) > 65536:
            raise ValueError('configuration too large')
        return value


def enabled():
    try:
        data = read_file(PREFS)
    except FileNotFoundError:
        return True
    for line in data.splitlines():
        line = line.strip()
        if line and not line.startswith(b'#') and b'=' in line:
            key, value = line.split(b'=', 1)
            if key.strip() == b'wifi.enabled':
                return value.strip() != b'0'
    return True


def unescape(value):
    out = bytearray()
    i = 0
    while i < len(value):
        char = value[i]
        i += 1
        if char == 92:
            if i == len(value):
                raise ValueError('incomplete escape')
            char = value[i]
            i += 1
            if char in (120, 88):
                pair = value[i:i + 2]
                if len(pair) != 2 or not re.fullmatch(b'[0-9a-fA-F]{2}', pair):
                    raise ValueError('invalid hex escape')
                char = int(pair, 16)
                i += 2
            else:
                char = {116: 9, 110: 10, 114: 13}.get(char, char)
        out.append(char)
    return bytes(out)


def network(ssid, password, encrypted, raw_psk=False):
    if not 1 <= len(ssid) <= 32 or b'\0' in ssid:
        raise ValueError('invalid SSID')
    if not encrypted:
        return ssid, None
    if raw_psk or len(password) == 64:
        if not re.fullmatch(b'[0-9a-fA-F]{64}', password):
            raise ValueError('invalid raw PSK')
        return ssid, password.decode('ascii').lower()
    if not 8 <= len(password) <= 63 or b'\0' in password:
        raise ValueError('invalid passphrase')
    return ssid, hashlib.pbkdf2_hmac('sha1', password, ssid, 4096, 32).hex()


def parse_profiles(data):
    result = []
    for line in data.splitlines():
        fields = line.split(b'\t')
        if len(fields) != 4 or fields[0] != b'v1' or fields[1] not in (b'0', b'1'):
            continue
        try:
            item = network(unescape(fields[2]), unescape(fields[3]), fields[1] == b'1')
        except ValueError:
            continue
        if item[0] not in [entry[0] for entry in result]:
            result.append(item)
        if len(result) == 12:
            break
    return result


def parse_live(data):
    """Read the small wpa config subset emitted by the upstream UI."""
    result = []
    current = None
    for line in data.splitlines():
        line = line.strip()
        if line == b'network={':
            if current is not None:
                raise ValueError('nested network')
            current = {}
        elif line == b'}' and current is not None:
            ssid = current.get(b'ssid', b'')
            if ssid.startswith(b'"') and ssid.endswith(b'"'):
                ssid = unescape(ssid[1:-1])
            elif re.fullmatch(b'(?:[0-9a-fA-F]{2}){1,32}', ssid):
                ssid = bytes.fromhex(ssid.decode('ascii'))
            else:
                raise ValueError('invalid SSID encoding')
            psk = current.get(b'psk', b'')
            raw = not (psk.startswith(b'"') and psk.endswith(b'"'))
            if not raw:
                psk = unescape(psk[1:-1])
            open_network = current.get(b'key_mgmt') == b'NONE'
            if not open_network and not psk:
                raise ValueError('unsupported authentication')
            result.append(network(ssid, psk, not open_network, raw))
            current = None
        elif current is not None and b'=' in line and not line.startswith(b'#'):
            key, value = line.split(b'=', 1)
            key = key.strip()
            if key in current:
                raise ValueError('duplicate network key')
            current[key] = value.strip()
    if current is not None:
        raise ValueError('unterminated network')
    return result


def render_config(networks):
    lines = ['# Migrated Wi-Fi credentials; keep root-readable only.',
             'ctrl_interface=/var/run/wpa_supplicant', 'ap_scan=1', 'update_config=0']
    for i, (ssid, psk) in enumerate(networks):
        lines.extend(['network={', '    ssid=' + ssid.hex(), '    scan_ssid=1',
                      '    priority=' + str(len(networks) - i),
                      '    psk=' + psk if psk else '    key_mgmt=NONE', '}'])
    return ('\n'.join(lines) + '\n').encode('ascii')


def atomic_private(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.' + path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            os.fchmod(stream.fileno(), 0o600)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def prepare():
    if not enabled():
        print('Wi-Fi disabled by saved preference')
        return 3
    if CONFIG.exists() or CONFIG.is_symlink():
        # Preserve administrator configuration; reject symlinks and broad modes.
        read_file(CONFIG)
        fd = os.open(CONFIG, os.O_RDONLY | os.O_NOFOLLOW)
        try:
            os.fchmod(fd, 0o600)
        finally:
            os.close(fd)
        print('Persistent Wi-Fi configuration is ready')
        return 0
    networks = []
    for path, parser in ((LIVE_CONFIG, parse_live), (PROFILES, parse_profiles)):
        try:
            networks = parser(read_file(path))
        except (FileNotFoundError, ValueError):
            continue
        if networks:
            break
    if not networks:
        print('No supported saved Wi-Fi profile is available')
        return 4
    atomic_private(CONFIG, render_config(networks))
    print('Saved Wi-Fi profile migrated to persistent configuration')
    return 0


def invoke(args, timeout=8):
    # WPA may include SSIDs in diagnostics: never pass its output to our log.
    try:
        return subprocess.run(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None


def processes():
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            args = (proc / 'cmdline').read_bytes().split(b'\0')
            yield int(proc.name), [a.decode('utf-8', 'surrogateescape') for a in args if a]
        except (OSError, ValueError):
            continue


def uses_interface(args):
    return any(arg == '-i' and i + 1 < len(args) and args[i + 1] == IFACE
               or arg == '-i' + IFACE for i, arg in enumerate(args))


def identity(pid):
    try:
        data = Path('/proc', str(pid), 'stat').read_text()
        # Everything after the final ')' starts at field 3; field 22 is starttime.
        return data.rsplit(')', 1)[1].split()[19]
    except (OSError, IndexError):
        return None


def remember(role, pid):
    token = identity(pid)
    if pid <= 1 or token is None:
        return False
    atomic_private(RUN / (role + '.json'), json.dumps({'pid': pid, 'start': token}).encode())
    return True


def await_daemon(role, pidfile):
    # A daemonizing parent can exit before its child writes the PID file.
    # Complete ownership even after cancellation so stop can collect that child.
    deadline = time.monotonic() + DAEMON_PID_WAIT
    while True:
        try:
            pid = int(read_file(pidfile).strip())
        except (OSError, ValueError):
            pid = 0
        if pid > 1 and remember(role, pid):
            return True
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(0.05, remaining))


def owned(role):
    try:
        state = json.loads(read_file(RUN / (role + '.json')))
        pid = state['pid']
        if type(pid) is not int:
            return None
        return pid if pid > 1 and identity(pid) == state['start'] else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


def other_manager():
    our_wpa = owned('wpa')
    for pid, args in processes():
        if not args:
            continue
        name = Path(args[0]).name
        if name in ('NetworkManager', 'connmand', 'iwd'):
            return True
        if name == 'wpa_supplicant' and pid != our_wpa and uses_interface(args):
            return True
    return False


def request_stop(_signum, _frame):
    global STOP_REQUESTED
    STOP_REQUESTED = True


def connect():
    if STOP_REQUESTED or not enabled():
        return 0
    if other_manager():
        print('Wi-Fi is already managed; existing connection preserved', flush=True)
        return 0
    for _ in range(INTERFACE_WAIT):
        if STOP_REQUESTED:
            return 0
        if Path('/sys/class/net', IFACE).exists():
            break
        time.sleep(1)
    else:
        print('Wi-Fi interface unavailable after waiting', flush=True)
        return 1
    if STOP_REQUESTED or other_manager():
        return 0
    if not owned('wpa'):
        # Also detect an existing manager whose command line does not name -i.
        result = invoke(['wpa_cli', '-i', IFACE, 'ping'])
        if result and result.returncode == 0 and b'PONG' in result.stdout:
            print('Existing WPA control interface preserved', flush=True)
            return 0
        if STOP_REQUESTED:
            return 0
        result = invoke(['ip', 'link', 'set', IFACE, 'up'])
        if result is None or result.returncode:
            print('Cannot bring up Wi-Fi interface', flush=True)
            return 1
        if STOP_REQUESTED:
            return 0
        wpa_pid = RUN / 'wpa.pid'
        wpa_pid.unlink(missing_ok=True)
        Path('/var/run/wpa_supplicant').mkdir(parents=True, exist_ok=True)
        result = invoke(['wpa_supplicant', '-B', '-P', str(wpa_pid), '-i', IFACE,
                         '-D', 'nl80211,wext', '-c', str(CONFIG)])
        # Even a cancelled or timed-out invocation may already have forked.
        # Record its daemon before observing cancellation so stop can own it.
        tracked = await_daemon('wpa', wpa_pid)
        if result is None or result.returncode or not tracked:
            print('Could not start Wi-Fi supplicant', flush=True)
            return 1
    if STOP_REQUESTED:
        return 0
    associated = False
    for _ in range(ASSOCIATION_WAIT):
        if STOP_REQUESTED:
            return 0
        result = invoke(['wpa_cli', '-i', IFACE, 'status'])
        if result and result.returncode == 0 and b'wpa_state=COMPLETED' in result.stdout.splitlines():
            associated = True
            break
        if STOP_REQUESTED:
            return 0
        time.sleep(1)
    if STOP_REQUESTED:
        return 0
    # Keep DHCP alive after this bounded worker exits, including when the AP
    # comes online later. udhcpc -b backgrounds if its initial attempts fail.
    if owned('dhcp') or any(args and Path(args[0]).name == 'udhcpc' and uses_interface(args)
                            for _, args in processes()):
        print('Existing Wi-Fi DHCP client preserved', flush=True)
        return 0
    script = next((p for p in (Path('/etc/udhcpc.script'), Path('/usr/share/udhcpc/default.script'))
                   if p.is_file() and os.access(p, os.X_OK)), None)
    if script is None:
        print('No BSP DHCP configuration script found', flush=True)
        return 1
    if STOP_REQUESTED:
        return 0
    dhcp_pid = RUN / 'dhcp.pid'
    dhcp_pid.unlink(missing_ok=True)
    result = invoke(['udhcpc', '-b', '-t', '5', '-T', '3', '-p', str(dhcp_pid),
                     '-i', IFACE, '-s', str(script)], timeout=25)
    tracked = await_daemon('dhcp', dhcp_pid)
    if result is None or result.returncode or not tracked:
        print('Could not start Wi-Fi DHCP client', flush=True)
        return 1
    if STOP_REQUESTED:
        return 0
    print('Wi-Fi boot connection started' if associated else
          'Wi-Fi connection pending; supplicant and DHCP continue in background', flush=True)
    return 0


def start():
    if other_manager():
        # Still persist credentials while the old UI is running, without
        # touching its daemons; the configuration will be ready for next boot.
        result = prepare()
        print('Existing network manager preserved')
        return 0 if result in (0, 3, 4) else result
    result = prepare()
    if result in (3, 4):
        return 0
    if result:
        return result
    RUN.mkdir(mode=0o700, parents=True, exist_ok=True)
    lock = os.open(RUN / 'worker.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(lock)
        print('Wi-Fi connection worker is already running')
        return 0
    sys.stdout.flush()
    sys.stderr.flush()
    ready_read, ready_write = os.pipe()
    pid = os.fork()
    if pid:
        os.close(ready_write)
        os.close(lock)
        try:
            readable, _, _ = select.select([ready_read], [], [], READY_WAIT)
            ready = bool(readable) and os.read(ready_read, 1) == b'1'
        finally:
            os.close(ready_read)
        if not ready:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            print('Wi-Fi connection worker did not initialize')
            return 1
        print('Wi-Fi connection scheduled')
        return 0
    os.close(ready_read)
    signal.signal(signal.SIGTERM, request_stop)
    try:
        os.setsid()
        LOG.parent.mkdir(parents=True, exist_ok=True)
        out = os.open(LOG, os.O_CREAT | os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW, 0o600)
        os.fchmod(out, 0o600)
        null = os.open('/dev/null', os.O_RDONLY)
        os.dup2(null, 0)
        os.dup2(out, 1)
        os.dup2(out, 2)
        os.close(null)
        os.close(out)
        if not remember('worker', os.getpid()):
            raise RuntimeError('worker identity unavailable')
        # Publish identity and install cancellation before start can succeed.
        os.write(ready_write, b'1')
        os.close(ready_write)
        ready_write = None
        result = connect()
    except Exception:
        # Exception strings can include input data; emit no credentials.
        print('Wi-Fi connection worker failed', flush=True)
        result = 1
    finally:
        if ready_write is not None:
            os.close(ready_write)
        (RUN / 'worker.json').unlink(missing_ok=True)
        os.close(lock)
    os._exit(result)


def stop():
    for role in ('worker', 'dhcp', 'wpa'):
        pid = owned(role)
        if pid:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            # Let the worker finish a bounded in-flight daemon launch and
            # record the child before stopping the recorded daemons below.
            timeout = WORKER_STOP_WAIT if role == 'worker' else 5
            for _ in range(int(timeout * 10)):
                if not owned(role):
                    break
                time.sleep(0.1)
            else:
                print('Owned Wi-Fi service did not stop; process record retained')
                return 1
        (RUN / (role + '.json')).unlink(missing_ok=True)
    print('Owned Wi-Fi services stopped')
    return 0


def main():
    os.umask(0o077)
    if len(sys.argv) != 2 or sys.argv[1] not in ('prepare', 'start', 'stop', 'status', 'restart'):
        print('Usage: k230-network.py {prepare|start|stop|status|restart}', file=sys.stderr)
        return 2
    action = sys.argv[1]
    try:
        if action == 'prepare':
            return prepare()
        if action == 'start':
            return start()
        if action == 'stop':
            return stop()
        if action == 'restart':
            if stop():
                return 1
            return start()
        active = bool(owned('worker') or owned('wpa') or other_manager())
        print('Wi-Fi service active' if active else 'Wi-Fi service stopped')
        return 0 if active else 3
    except (OSError, ValueError):
        print('Wi-Fi configuration or service operation failed; inspect file permissions and format', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())

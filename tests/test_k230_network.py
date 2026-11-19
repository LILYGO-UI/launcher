#!/usr/bin/env python3
"""Credential migration and isolated K230 Wi-Fi service regression checks."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('k230_network', ROOT / 'packaging/buildroot/k230-network.py')
network = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(network)


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='lilygo-k230-network-')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for name in ('CONFIG', 'PREFS', 'PROFILES', 'LIVE_CONFIG', 'RUN', 'LOG'):
            patch = mock.patch.object(network, name, self.root / name.lower())
            patch.start()
            self.addCleanup(patch.stop)
        patch = mock.patch.object(network, 'STOP_REQUESTED', False)
        patch.start()
        self.addCleanup(patch.stop)

    def quiet(self, action):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = action()
        return result, output.getvalue()

    def test_password_and_ssid_escaped_without_disclosure(self):
        network.PROFILES.write_bytes(b'v1\t1\tTea\\tRoom\\\\\\x22\tpass\\\\word\n')
        result, output = self.quiet(network.prepare)
        self.assertEqual(result, 0)
        ssid = b'Tea\tRoom\\"'
        password = b'pass\\word'
        config = network.CONFIG.read_text()
        self.assertIn('ssid=' + ssid.hex(), config)
        self.assertIn('psk=' + hashlib.pbkdf2_hmac('sha1', password, ssid, 4096, 32).hex(), config)
        self.assertNotIn('pass', output + config)
        self.assertNotIn('Tea', output + config)
        self.assertEqual(network.CONFIG.stat().st_mode & 0o777, 0o600)

    def test_open_and_raw_psk_and_profile_priority(self):
        psk = b'AB' * 32
        data = b'v1\t0\tGuest\t\n' + b'v1\t1\tSecure\t' + psk + b'\n'
        items = network.parse_profiles(data)
        self.assertEqual(items, [(b'Guest', None), (b'Secure', psk.decode().lower())])
        config = network.render_config(items)
        self.assertIn(b'key_mgmt=NONE', config)
        self.assertIn(b'priority=2', config)
        self.assertNotIn(psk, config)

    def test_invalid_profiles_are_not_imported(self):
        data = (b'v2\t1\tSSID\tpassword\n' + b'v1\t1\tSSID\tshort\n' +
                b'v1\t1\t\tpassword\n' + b'v1\t1\tbad\\xNN\tpassword\n' +
                b'v1\t1\t' + b'x' * 33 + b'\tpassword\n' +
                b'v1\t1\tSSID\t' + b'z' * 64 + b'\n' +
                b'v1\t1\tSSID\\x00\tpassword\n')
        self.assertEqual(network.parse_profiles(data), [])
        network.PROFILES.write_bytes(data)
        result, output = self.quiet(network.prepare)
        self.assertEqual(result, 4)
        self.assertFalse(network.CONFIG.exists())
        self.assertNotIn('password', output)

    def test_live_config_preferred_and_psk_derived(self):
        network.PROFILES.write_bytes(b'v1\t0\tOther\t\n')
        network.LIVE_CONFIG.write_bytes(b'ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n'
                                        b' ssid="Test\\x20Network"\n psk="secret12"\n}\n')
        self.assertEqual(self.quiet(network.prepare)[0], 0)
        text = network.CONFIG.read_text()
        self.assertIn(b'Test Network'.hex(), text)
        self.assertNotIn(b'Other'.hex(), text)
        self.assertNotIn('secret12', text)
        self.assertEqual(network.parse_live(text.encode()),
                         network.parse_live(network.LIVE_CONFIG.read_bytes()))

    def test_malformed_live_config_falls_back(self):
        network.LIVE_CONFIG.write_bytes(b'network={\nssid="broken"\n')
        network.PROFILES.write_bytes(b'v1\t0\tGuest\t\n')
        self.assertEqual(self.quiet(network.prepare)[0], 0)
        self.assertIn(b'Guest'.hex(), network.CONFIG.read_text())

    def test_preferences_and_existing_config(self):
        network.PREFS.write_text('# prefs\n wifi.enabled = 0 \n')
        network.PROFILES.write_bytes(b'v1\t0\tGuest\t\n')
        self.assertEqual(self.quiet(network.prepare)[0], 3)
        self.assertFalse(network.CONFIG.exists())
        network.PREFS.write_text('wifi.enabled=1\n')
        network.CONFIG.write_text('# administrator-owned\n')
        network.CONFIG.chmod(0o644)
        self.assertEqual(self.quiet(network.prepare)[0], 0)
        self.assertEqual(network.CONFIG.read_text(), '# administrator-owned\n')
        self.assertEqual(network.CONFIG.stat().st_mode & 0o777, 0o600)

    def test_config_symlink_is_refused(self):
        other = self.root / 'other'
        other.write_text('must not be modified')
        other.chmod(0o644)
        network.CONFIG.symlink_to(other)
        with self.assertRaises(OSError):
            network.prepare()
        self.assertEqual(other.stat().st_mode & 0o777, 0o644)

    def test_stale_process_records_never_kill_unrelated_processes(self):
        network.RUN.mkdir()
        (network.RUN / 'wpa.json').write_text('{"pid": 1234, "start": "old"}')
        with mock.patch.object(network, 'identity', return_value='new'), mock.patch.object(network.os, 'kill') as kill:
            self.quiet(network.stop)
            kill.assert_not_called()

    def test_non_integer_process_records_are_ignored(self):
        network.RUN.mkdir()
        for value in ('1e999', '12.5', 'true', '"1234"', 'null'):
            (network.RUN / 'worker.json').write_text('{"pid": ' + value + ', "start": "100"}')
            with mock.patch.object(network.os, 'kill') as kill:
                self.assertEqual(self.quiet(network.stop)[0], 0)
                kill.assert_not_called()

    def test_daemon_pid_wait_requires_a_live_identity(self):
        network.RUN.mkdir()
        pidfile = network.RUN / 'wpa.pid'
        pidfile.write_text('4001\n')
        with mock.patch.object(network, 'identity', side_effect=[None, None, '100']), \
             mock.patch.object(network, 'DAEMON_PID_WAIT', 0.5):
            self.assertTrue(network.await_daemon('wpa', pidfile))
        self.assertEqual(json.loads((network.RUN / 'wpa.json').read_text()),
                         {'pid': 4001, 'start': '100'})

    def test_daemon_pid_wait_rejects_missing_invalid_and_dead_pids(self):
        network.RUN.mkdir()
        pidfile = network.RUN / 'wpa.pid'
        for value in (None, '', 'invalid', '0', '1', '-1', '4001'):
            with self.subTest(value=value):
                if value is not None:
                    pidfile.write_text(value)
                started = time.monotonic()
                with mock.patch.object(network, 'identity', return_value=None), \
                     mock.patch.object(network, 'DAEMON_PID_WAIT', 0.06):
                    self.assertFalse(network.await_daemon('wpa', pidfile))
                self.assertLess(time.monotonic() - started, 0.5)
                self.assertFalse((network.RUN / 'wpa.json').exists())

    def test_start_returns_before_worker_and_refuses_duplicate_worker(self):
        network.CONFIG.write_text('# configured\n')
        source = ROOT / 'packaging/buildroot/k230-network.py'
        runner = f'''
import importlib.util, os, sys, time
from pathlib import Path
spec = importlib.util.spec_from_file_location('network', {str(source)!r})
n = importlib.util.module_from_spec(spec); spec.loader.exec_module(n)
n.CONFIG = Path({str(network.CONFIG)!r})
n.PREFS = Path({str(network.PREFS)!r})
n.RUN = Path({str(network.RUN)!r})
n.LOG = Path({str(network.LOG)!r})
n.other_manager = lambda: False
n.identity = lambda pid: str(pid)
n.connect = lambda: (time.sleep(2) or 0)
sys.exit(n.start())
'''
        started = time.monotonic()
        first = subprocess.run([sys.executable, '-c', runner], capture_output=True, text=True, timeout=4)
        self.assertLess(time.monotonic() - started, 1.5)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn('scheduled', first.stdout)
        self.assertTrue((network.RUN / 'worker.json').exists())
        second = subprocess.run([sys.executable, '-c', runner], capture_output=True, text=True, timeout=4)
        self.assertIn('already running', second.stdout)
        # Wait for this deliberately short fixture worker to release its lock.
        deadline = time.monotonic() + 4
        while (network.RUN / 'worker.json').exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse((network.RUN / 'worker.json').exists())

    def test_start_waits_for_ownership_then_stop_allows_inflight_work_to_finish(self):
        network.CONFIG.write_text('# configured\n')
        source = ROOT / 'packaging/buildroot/k230-network.py'
        inflight = self.root / 'inflight'
        finished = self.root / 'finished'
        runner = f'''
import importlib.util, os, sys, time
from pathlib import Path
spec = importlib.util.spec_from_file_location('network', {str(source)!r})
n = importlib.util.module_from_spec(spec); spec.loader.exec_module(n)
n.CONFIG = Path({str(network.CONFIG)!r})
n.PREFS = Path({str(network.PREFS)!r})
n.RUN = Path({str(network.RUN)!r})
n.LOG = Path({str(network.LOG)!r})
n.other_manager = lambda: False
n.identity = lambda pid: str(pid)
remember = n.remember
def delayed_remember(role, pid):
    if role == 'worker': time.sleep(0.2)
    return remember(role, pid)
n.remember = delayed_remember
def connect():
    Path({str(inflight)!r}).touch()
    time.sleep(0.4)
    if n.STOP_REQUESTED: Path({str(finished)!r}).touch()
    return 0
n.connect = connect
sys.exit(n.start() if sys.argv[1] == 'start' else n.stop())
'''
        first = subprocess.run([sys.executable, '-c', runner, 'start'],
                               capture_output=True, text=True, timeout=4)
        self.assertEqual(first.returncode, 0, first.stderr)
        record = network.RUN / 'worker.json'
        self.assertTrue(record.exists(), 'start must publish worker ownership before returning')
        deadline = time.monotonic() + 2
        while not inflight.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(inflight.exists())
        stopped = subprocess.run([sys.executable, '-c', runner, 'stop'],
                                 capture_output=True, text=True, timeout=4)
        self.assertEqual(stopped.returncode, 0, stopped.stdout + stopped.stderr)
        self.assertTrue(finished.exists(), 'SIGTERM must let in-flight work finish before cleanup')
        self.assertFalse(record.exists())


class ServiceTests(unittest.TestCase):
    quiet = MigrationTests.quiet

    def setUp(self):
        MigrationTests.setUp(self)
        network.RUN.mkdir()
        network.CONFIG.write_text('network={\nssid=74657374\nkey_mgmt=NONE\n}\n')
        self.commands = self.root / 'commands.jsonl'
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.sysroot = self.root / 'sysroot'
        (self.sysroot / 'sys/class/net/wlan0').mkdir(parents=True)
        script = self.sysroot / 'etc/udhcpc.script'
        script.parent.mkdir(parents=True)
        script.write_text('#!/bin/sh\nexit 0\n')
        script.chmod(0o755)
        stub = '#!' + sys.executable + '\n' + '''
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
name = Path(sys.argv[0]).name
with open(os.environ['TEST_NETWORK_COMMANDS'], 'a') as f:
    f.write(json.dumps([name, args]) + '\\n')
if name == 'wpa_cli':
    if args[-1] == 'ping':
        if os.environ.get('TEST_EXTERNAL_WPA'): print('PONG')
        else: sys.exit(1)
    elif args[-1] == 'status': print('wpa_state=COMPLETED')
elif name == 'wpa_supplicant': Path(args[args.index('-P') + 1]).write_text('4001\\n')
elif name == 'udhcpc': Path(args[args.index('-p') + 1]).write_text('4002\\n')
'''
        for name in ('ip', 'wpa_cli', 'wpa_supplicant', 'udhcpc'):
            path = self.bin / name
            path.write_text(stub)
            path.chmod(0o755)
        env = mock.patch.dict(os.environ, PATH=str(self.bin) + ':' + os.environ['PATH'],
                              TEST_NETWORK_COMMANDS=str(self.commands))
        env.start()
        self.addCleanup(env.stop)
        self.process_patch = mock.patch.object(network, 'processes', return_value=[])
        self.processes = self.process_patch.start()
        self.addCleanup(self.process_patch.stop)
        identity = mock.patch.object(network, 'identity', return_value='100')
        identity.start()
        self.addCleanup(identity.stop)
        original_path = Path

        def paths(*parts):
            path = original_path(*parts)
            if str(path).startswith(('/sys/', '/var/run/', '/etc/udhcpc', '/usr/share/udhcpc')):
                return self.sysroot / str(path).lstrip('/')
            return path

        path_patch = mock.patch.object(network, 'Path', side_effect=paths)
        path_patch.start()
        self.addCleanup(path_patch.stop)

    def calls(self):
        return [json.loads(line) for line in self.commands.read_text().splitlines()] if self.commands.exists() else []

    def test_connection_owns_only_its_wpa_and_dhcp(self):
        self.assertEqual(self.quiet(network.connect)[0], 0)
        calls = self.calls()
        self.assertEqual([name for name, _ in calls], ['wpa_cli', 'ip', 'wpa_supplicant', 'wpa_cli', 'udhcpc'])
        self.assertEqual(network.owned('wpa'), 4001)
        self.assertEqual(network.owned('dhcp'), 4002)
        dhcp = calls[-1][1]
        self.assertIn('-b', dhcp)
        self.assertNotIn('-q', dhcp)  # Keep lease renewal and future association working.
        self.assertIn(str(self.sysroot / 'etc/udhcpc.script'), dhcp)
        self.assertEqual(self.quiet(network.connect)[0], 0)
        self.assertEqual(sum(name == 'wpa_supplicant' for name, _ in self.calls()), 1)
        self.assertEqual(sum(name == 'udhcpc' for name, _ in self.calls()), 1)

    def test_existing_managers_are_left_untouched(self):
        for args in (['/usr/sbin/NetworkManager'], ['wpa_supplicant', '-iwlan0'],
                     ['wpa_supplicant', '-i', 'wlan0']):
            self.processes.return_value = [(999, args)]
            self.assertEqual(self.quiet(network.connect)[0], 0)
            self.assertEqual(self.calls(), [])

    def test_existing_control_socket_is_left_untouched(self):
        with mock.patch.dict(os.environ, TEST_EXTERNAL_WPA='1'):
            self.assertEqual(self.quiet(network.connect)[0], 0)
        self.assertEqual([name for name, _ in self.calls()], ['wpa_cli'])

    def test_existing_dhcp_is_preserved(self):
        self.processes.return_value = [(999, ['udhcpc', '-i', 'wlan0'])]
        self.assertEqual(self.quiet(network.connect)[0], 0)
        self.assertFalse(any(name == 'udhcpc' for name, _ in self.calls()))

    def test_bsp_share_dhcp_hook_is_used_without_etc_hook(self):
        (self.sysroot / 'etc/udhcpc.script').unlink()
        hook = self.sysroot / 'usr/share/udhcpc/default.script'
        hook.parent.mkdir(parents=True)
        hook.write_text('#!/bin/sh\nexit 0\n')
        hook.chmod(0o755)
        self.assertEqual(self.quiet(network.connect)[0], 0)
        self.assertIn(str(hook), self.calls()[-1][1])

    def test_cancelled_inflight_supplicant_is_recorded_without_starting_dhcp(self):
        original = network.invoke

        def invoke(args, **kwargs):
            result = original(args, **kwargs)
            if args[0] == 'wpa_supplicant':
                network.request_stop(None, None)
            return result

        with mock.patch.object(network, 'invoke', side_effect=invoke):
            self.assertEqual(self.quiet(network.connect)[0], 0)
        self.assertEqual(network.owned('wpa'), 4001)
        self.assertFalse(any(name == 'udhcpc' for name, _ in self.calls()))

    def test_cancelled_inflight_dhcp_is_recorded(self):
        original = network.invoke

        def invoke(args, **kwargs):
            result = original(args, **kwargs)
            if args[0] == 'udhcpc':
                network.request_stop(None, None)
            return result

        with mock.patch.object(network, 'invoke', side_effect=invoke):
            self.assertEqual(self.quiet(network.connect)[0], 0)
        self.assertEqual(network.owned('wpa'), 4001)
        self.assertEqual(network.owned('dhcp'), 4002)

    def test_daemon_pid_files_can_arrive_after_invocation_returns(self):
        original = network.invoke
        writers = []

        def invoke(args, **kwargs):
            result = original(args, **kwargs)
            if args[0] in ('wpa_supplicant', 'udhcpc'):
                role, pid = ('wpa', 4001) if args[0] == 'wpa_supplicant' else ('dhcp', 4002)
                pidfile = network.RUN / (role + '.pid')
                pidfile.unlink()
                writer = threading.Timer(0.15, pidfile.write_text, args=(str(pid) + '\n',))
                writers.append(writer)
                writer.start()
            return result

        try:
            with mock.patch.object(network, 'invoke', side_effect=invoke):
                self.assertEqual(self.quiet(network.connect)[0], 0)
        finally:
            for writer in writers:
                writer.join()
        self.assertEqual(network.owned('wpa'), 4001)
        self.assertEqual(network.owned('dhcp'), 4002)

    def test_cancellation_still_waits_for_a_delayed_daemon_pid(self):
        original = network.invoke
        writers = []

        def invoke(args, **kwargs):
            result = original(args, **kwargs)
            if args[0] == 'wpa_supplicant':
                pidfile = network.RUN / 'wpa.pid'
                pidfile.unlink()
                network.request_stop(None, None)
                writer = threading.Timer(0.15, pidfile.write_text, args=('4001\n',))
                writers.append(writer)
                writer.start()
            return result

        try:
            with mock.patch.object(network, 'invoke', side_effect=invoke):
                self.assertEqual(self.quiet(network.connect)[0], 0)
        finally:
            for writer in writers:
                writer.join()
        self.assertEqual(network.owned('wpa'), 4001)
        self.assertFalse(any(name == 'udhcpc' for name, _ in self.calls()))

    def test_timed_out_launch_still_records_a_forked_daemon(self):
        original = network.invoke

        def invoke(args, **kwargs):
            result = original(args, **kwargs)
            return None if args[0] == 'wpa_supplicant' else result

        with mock.patch.object(network, 'invoke', side_effect=invoke):
            self.assertEqual(self.quiet(network.connect)[0], 1)
        self.assertEqual(network.owned('wpa'), 4001)
        self.assertFalse(any(name == 'udhcpc' for name, _ in self.calls()))

    def test_missing_interface_wait_is_bounded(self):
        (self.sysroot / 'sys/class/net/wlan0').rmdir()
        with mock.patch.object(network.time, 'sleep') as sleep:
            self.assertEqual(self.quiet(network.connect)[0], 1)
            self.assertEqual(sleep.call_count, network.INTERFACE_WAIT)
        self.assertEqual(self.calls(), [])

    def test_absent_access_point_still_leaves_reconnect_workers(self):
        original = network.invoke

        def invoke(args, **kwargs):
            if args[-1] == 'status':
                return subprocess.CompletedProcess(args, 0, b'wpa_state=DISCONNECTED\n')
            return original(args, **kwargs)

        with mock.patch.object(network, 'invoke', side_effect=invoke), \
             mock.patch.object(network, 'ASSOCIATION_WAIT', 2), \
             mock.patch.object(network.time, 'sleep') as sleep:
            result, output = self.quiet(network.connect)
            self.assertEqual(result, 0)
            self.assertEqual(sleep.call_count, 2)
            self.assertIn('pending', output)
        self.assertEqual(network.owned('wpa'), 4001)
        self.assertEqual(network.owned('dhcp'), 4002)

    def test_disabled_preference_does_not_start_network(self):
        network.PREFS.write_text('wifi.enabled=0\n')
        self.assertEqual(self.quiet(network.connect)[0], 0)
        self.assertEqual(self.calls(), [])

    def test_service_wrapper_syntax(self):
        subprocess.run(['/bin/sh', '-n', str(ROOT / 'packaging/buildroot/S45lilygo-ui-network')], check=True)


if __name__ == '__main__':
    unittest.main()

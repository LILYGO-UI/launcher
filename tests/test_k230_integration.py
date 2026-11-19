#!/usr/bin/env python3
"""Exercise K230 service/build entry points without devices or SDK binaries."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def executable(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)
    path.chmod(0o755)


def commands(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def test_service(base):
    base.mkdir()
    daemon = base / "lilygo-ui-launcher"
    pidfile = base / "launcher.pid"
    config = base / "launcher.conf"
    logfile = base / "launcher.log"
    upstream = base / "S99zz_k230_phone_ui"
    state = base / "process.json"
    calls = base / "commands.jsonl"
    flags = base / "flags"
    flags.mkdir()
    executable(daemon, "#!/bin/sh\nexit 0\n")
    source = (ROOT / "packaging/buildroot/S99lilygo-ui-launcher").read_text()
    for name, old, value in (
        ("DAEMON", "/usr/bin/lilygo-ui-launcher", daemon),
        ("PIDFILE", "/run/lilygo-ui-launcher.pid", pidfile),
        ("CONFIG", "/etc/lilygo/ui/launcher.conf", config),
        ("LOG_FILE", "/var/log/lilygo-ui-launcher.log", logfile),
        ("XDG_RUNTIME_DIR", "/run/lilygo-ui", base / "runtime"),
    ):
        source = source.replace(f"{name}={old}", f"{name}={shlex.quote(str(value))}")
    source = source.replace("/etc/init.d/S99zz_k230_phone_ui", shlex.quote(str(upstream)))
    service = base / "service"
    executable(service, source)
    config.write_text("DISPLAY_DEVICE=/dev/dri/test\nINPUT_DEVICE=auto\nORIENTATION=landscape\n")
    stubbin = base / "bin"
    executable(stubbin / "sleep", "#!/bin/sh\nexit 0\n")
    executable(stubbin / "pidof", f"#!/bin/sh\ntest -f {shlex.quote(str(flags / 'upstream'))}\n")
    executable(stubbin / "start-stop-daemon", f"#!{sys.executable}\n" + r'''
import json, os, sys
from pathlib import Path
a = sys.argv[1:]
calls, state, flags = (Path(os.environ[n]) for n in ('TEST_CALLS', 'TEST_STATE', 'TEST_FLAGS'))
with calls.open('a') as f: f.write(json.dumps(a) + '\n')
pidfile = Path(a[a.index('-p') + 1])
program = a[a.index('-x') + 1]
running = json.loads(state.read_text()) if state.exists() else None
matches = running and pidfile.exists() and pidfile.read_text().strip() == str(running['pid']) and running['exe'] == program
if '-S' in a:
    if matches: sys.exit(1)
    assert '-b' in a and '-m' in a and '-O' in a
    pidfile.write_text('4242\n')
    with Path(a[a.index('-O') + 1]).open('a') as f: f.write('launcher stderr retained\n')
    if not (flags / 'crash').exists(): state.write_text(json.dumps({'pid': 4242, 'exe': program}))
    sys.exit(0)
if '-t' in a: sys.exit(0 if matches else 1)
if not matches: sys.exit(1)
if not (flags / 'hang').exists(): state.unlink()
''')
    env = dict(os.environ, PATH=f"{stubbin}:{os.environ['PATH']}", TEST_CALLS=str(calls),
               TEST_STATE=str(state), TEST_FLAGS=str(flags))

    def run(action, expected=0):
        result = subprocess.run([str(service), action], env=env, text=True, capture_output=True)
        assert result.returncode == expected, (action, result.returncode, result.stdout, result.stderr)
        return result.stdout + result.stderr

    assert "stopped" in run("status", 3)
    run("stop")
    run("start")
    assert "running" in run("status")
    first_start = next(c for c in commands(calls) if '-S' in c)
    assert first_start[first_start.index('--') + 1:] == [
        '--display', 'drm', '--drm', '/dev/dri/test', '--input', 'auto', '--orientation', 'landscape']
    assert "stderr retained" in logfile.read_text()
    assert logfile.stat().st_mode & 0o777 == 0o600
    run("start")
    assert sum('-S' in c for c in commands(calls)) == 1
    run("restart")
    assert sum('-S' in c for c in commands(calls)) == 2
    run("stop")
    run("stop")
    assert not pidfile.exists()

    # A PID reused by a different command must not receive a stop signal.
    pidfile.write_text('4242\n')
    state.write_text(json.dumps({'pid': 4242, 'exe': '/unrelated/process'}))
    before = sum('-K' in c and '-t' not in c for c in commands(calls))
    run("stop")
    assert state.exists() and not pidfile.exists()
    assert sum('-K' in c and '-t' not in c for c in commands(calls)) == before
    state.unlink()
    for invalid in ('1\n', 'not-a-pid\n'):
        pidfile.write_text(invalid)
        run("stop")
        assert not pidfile.exists()

    executable(upstream, '#!/bin/sh\nexit 0\n')
    assert "Stop and disable" in run("start", 1)
    upstream.unlink()
    (flags / 'upstream').touch()
    run("start", 1)
    (flags / 'upstream').unlink()
    (flags / 'crash').touch()
    assert "failed to start" in run("start", 1)
    assert not pidfile.exists()
    (flags / 'crash').unlink()
    run("start")
    (flags / 'hang').touch()
    assert "did not stop" in run("stop", 1)
    assert pidfile.exists() and state.exists()
    (flags / 'hang').unlink()
    run("stop")


def test_builder(base):
    base.mkdir()
    sdk = base / 'SDK output'
    toolchainfile = sdk / 'host/share/buildroot/toolchainfile.cmake'
    toolchainfile.parent.mkdir(parents=True)
    toolchainfile.touch()
    appkit = base / 'AppKit source'
    launcher = base / 'Launcher source'
    app = base / 'Independent app'
    for project in (appkit, launcher, app):
        project.mkdir()
        (project / 'CMakeLists.txt').write_text('# fixture\n')
    toolchain = appkit / 'cmake/k230-toolchain.cmake'
    toolchain.parent.mkdir()
    toolchain.touch()
    script = launcher / 'tools/build-k230.sh'
    executable(script, (ROOT / 'tools/build-k230.sh').read_text())
    calls = base / 'commands.jsonl'
    stubbin = base / 'bin'
    executable(stubbin / 'lpm', '#!/bin/sh\nexit 0\n')
    stub = f'#!{sys.executable}\n' + r'''
import json, os, sys
from pathlib import Path
a = sys.argv[1:]
with Path(os.environ['TEST_CALLS']).open('a') as f:
    f.write(json.dumps([Path(sys.argv[0]).name, a, os.environ.get('LILYGO_UI_K230_SDK')]) + '\n')
if '-S' in a:
    build = Path(a[a.index('-B') + 1]); build.mkdir(parents=True, exist_ok=True)
    sdk = next(v.split('=', 1)[1] for v in a if v.startswith('-DLILYGO_UI_K230_SDK='))
    toolchain = next(v.split('=', 1)[1] for v in a if v.startswith('-DCMAKE_TOOLCHAIN_FILE='))
    (build / 'CMakeCache.txt').write_text('LILYGO_UI_K230_SDK:PATH=' + sdk + '\nCMAKE_TOOLCHAIN_FILE:FILEPATH=' + toolchain + '\n')
if '--build' in a and os.environ.get('FAIL_BUILD'): sys.exit(7)
'''
    executable(stubbin / 'cmake', stub)
    executable(stubbin / 'cpack', stub)
    env = dict(os.environ, PATH=f"{stubbin}:{os.environ['PATH']}", TEST_CALLS=str(calls), BUILD_JOBS='3')

    def run(arguments, expected=0, **extra):
        result = subprocess.run([str(script), *map(str, arguments)], env=dict(env, **extra),
                                text=True, capture_output=True)
        assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
        return result.stdout + result.stderr

    run([sdk, appkit, app])
    trace = commands(calls)
    assert len(trace) == 9
    for i, project in enumerate((appkit, launcher, app)):
        configure, build, package = trace[3*i:3*i+3]
        assert configure[1][configure[1].index('-S') + 1] == str(project)
        assert configure[2] == str(sdk)
        for flag in ('-DLILYGO_UI_PLATFORM=k230', '-DLILYGO_UI_TARGET_OS=buildroot',
                     '-DLILYGO_UI_RENDERER=software', '-DCM0_SIMULATOR=OFF'):
            assert flag in configure[1]
        assert ('-DLAUNCHER_SYSTEM_SESSION=OFF' in configure[1]) == (project == launcher)
        assert build[1][-2:] == ['--parallel', '3']
        assert package[0] == 'cpack' and package[1][package[1].index('-G') + 1] == 'TGZ'
    calls.unlink()
    run([sdk, appkit])
    assert len(commands(calls)) == 6
    calls.unlink()
    run([sdk, appkit, app], 7, FAIL_BUILD='1')
    assert len(commands(calls)) == 2  # No package or later projects after a failed build.
    calls.unlink()
    run([sdk, appkit, app], 2, BUILD_JOBS='0')
    assert not calls.exists()
    cache = app / 'build/k230-cross/CMakeCache.txt'
    cache.write_text('LILYGO_UI_K230_SDK:PATH=/different-sdk\n')
    assert 'different SDK/toolchain' in run([sdk, appkit, app], 1)
    assert not calls.exists()  # Validate every requested project before building any.


with tempfile.TemporaryDirectory(prefix='lilygo-k230-integration-') as temporary:
    root = Path(temporary)
    test_service(root / 'service')
    test_builder(root / 'builder')
print('K230 init service and SDK build helper passed')

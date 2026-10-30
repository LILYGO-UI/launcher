#!/bin/sh
# All privileged commands are fixtures; never touches host packages/services.
set -eu
helper=${1:?update helper path required}
fixture=$(mktemp -d "${TMPDIR:-/tmp}/launcher-setup-update.XXXXXX")
trap 'rm -rf "$fixture"' EXIT HUP INT TERM
export LILYGO_TEST_UPDATE_FIXTURE="$fixture"
export LILYGO_UI_LAUNCHER_STATE_DIR="$fixture/state"
mkdir -m 0700 "$fixture/state"
mkdir "$fixture/bin"
export PATH="$fixture/bin:$PATH"
printf 'inactive\n' > "$fixture/service"
cat > "$fixture/bin/id" <<'STUB'
#!/bin/sh
[ "$1" = -u ] || exit 1
printf '%s\n' "${LILYGO_TEST_UID:-0}"
STUB
cat > "$fixture/bin/stat" <<'STUB'
#!/bin/sh
[ "$1" = -c ] && [ "$2" = '%u:%a' ] || exit 1
case "$(uname -s)" in
    Darwin) mode=$(/usr/bin/stat -f '%Lp' "$3") ;;
    *) mode=$(/usr/bin/stat -c '%a' "$3") ;;
esac
printf '0:%s\n' "$mode"
STUB
cat > "$fixture/bin/flock" <<'STUB'
#!/bin/sh
[ ! -e "$LILYGO_TEST_UPDATE_FIXTURE/busy" ] || exit 1
if [ "${LILYGO_TEST_REAL_FLOCK:-0}" = 1 ]; then exec /usr/bin/flock "$@"; fi
STUB
cat > "$fixture/bin/systemctl" <<'STUB'
#!/bin/sh
printf 'systemctl %s\n' "$*" >> "$LILYGO_TEST_UPDATE_FIXTURE/commands"
case "$*" in
    'show --property=ActiveState --value lilygo-ui-launcher-first-boot-update.service')
        [ ! -e "$LILYGO_TEST_UPDATE_FIXTURE/show-fail" ] || exit 1
        cat "$LILYGO_TEST_UPDATE_FIXTURE/service" ;;
    'show --property=Job --value lilygo-ui-launcher-first-boot-update.service')
        if [ -f "$LILYGO_TEST_UPDATE_FIXTURE/job" ]; then cat "$LILYGO_TEST_UPDATE_FIXTURE/job"; fi ;;
    'start --no-block lilygo-ui-launcher-first-boot-update.service')
        [ ! -e "$LILYGO_TEST_UPDATE_FIXTURE/start-fail" ] || exit 1
        printf 'activating\n' > "$LILYGO_TEST_UPDATE_FIXTURE/service" ;;
    *) exit 2 ;;
esac
STUB
cat > "$fixture/bin/dpkg" <<'STUB'
#!/bin/sh
printf 'dpkg %s\n' "$*" >> "$LILYGO_TEST_UPDATE_FIXTURE/commands"
[ "$*" = '--force-confdef --force-confold --configure -a' ] || exit 2
[ ! -e "$LILYGO_TEST_UPDATE_FIXTURE/repair-fail" ]
STUB
cat > "$fixture/bin/apt-get" <<'STUB'
#!/bin/sh
printf 'apt-get %s\n' "$*" >> "$LILYGO_TEST_UPDATE_FIXTURE/commands"
[ "$DEBIAN_FRONTEND" = noninteractive ] && [ "$APT_LISTCHANGES_FRONTEND" = none ] || exit 2
case " $* " in
    *' update ')
        case " $* " in *' APT::Update::Error-Mode=any '*) ;; *) exit 2 ;; esac
        [ ! -e "$LILYGO_TEST_UPDATE_FIXTURE/refresh-fail" ] || exit 100 ;;
    *' -s '*) printf 'Inst package-one [1] (2 stable)\nInst package-two [1] (2 stable)\n' ;;
    *' -y '*)
        printf 'dlstatus:1:25.5:Downloading\npmstatus:package-one:60.1:Unpacking\npmstatus:package-two:100:Done\n' >&3
        [ ! -e "$LILYGO_TEST_UPDATE_FIXTURE/install-fail" ] || exit 100
        if [ -e "$LILYGO_TEST_UPDATE_FIXTURE/install-hold" ]; then
            touch "$LILYGO_TEST_UPDATE_FIXTURE/install-waiting"
            while [ -e "$LILYGO_TEST_UPDATE_FIXTURE/install-hold" ]; do sleep 0.1; done
        fi ;;
    *) exit 2 ;;
esac
STUB
chmod +x "$fixture/bin/"*
job="$fixture/state/first-boot-update"
must_fail() {
    if sh "$helper" "$1" > "$fixture/output" 2>&1; then
        echo "Unexpected success: $1" >&2; exit 1
    fi
}
status_is() {
    sh "$helper" status > "$fixture/output"
    grep -Fx "status=$1" "$fixture/output" >/dev/null
}
status_is idle
sh "$helper" check
status_is queued
# A service job may be queued before its ActiveState changes.
printf 'inactive\n' > "$fixture/service"
printf '42\n' > "$fixture/job"
status_is queued
must_fail install
rm "$fixture/job"
printf 'activating\n' > "$fixture/service"
# Type=oneshot is activating for the entire run, never 'active'.
must_fail install
sh "$helper" worker
printf 'inactive\n' > "$fixture/service"
status_is available
grep -Fx 'packages=2' "$fixture/output" >/dev/null
! grep '^dpkg ' "$fixture/commands" >/dev/null

# Installation is owned by a separate job; all status can be read by a new caller.
sh "$helper" install
sh "$helper" worker
printf 'inactive\n' > "$fixture/service"
status_is succeeded
grep -Fx 'percent=100' "$fixture/output" >/dev/null
grep -Fx 'dpkg --force-confdef --force-confold --configure -a' "$fixture/commands" >/dev/null
[ ! -e "$job/progress" ]
[ "$(stat -c '%u:%a' "$job/status")" = 0:600 ]

# Failed refresh does not run an installation against old indexes.
printf '' > "$fixture/commands"
touch "$fixture/refresh-fail"
sh "$helper" install
must_fail worker
printf 'inactive\n' > "$fixture/service"
status_is failed
! grep 'apt-get -y' "$fixture/commands" >/dev/null
rm "$fixture/refresh-fail"

# Retrying re-enters a fresh worker and repairs interrupted dpkg state.
touch "$fixture/install-fail"
sh "$helper" install
must_fail worker
printf 'inactive\n' > "$fixture/service"
status_is failed
rm "$fixture/install-fail"
sh "$helper" install
sh "$helper" worker
printf 'inactive\n' > "$fixture/service"
status_is succeeded

# A transport failure is not proof the job died; only inactive/failed is terminal.
sh "$helper" check
status_is queued
touch "$fixture/show-fail"
must_fail status
rm "$fixture/show-fail"
printf 'inactive\n' > "$fixture/service"
status_is failed
grep -Fx 'message=interrupted' "$fixture/output" >/dev/null

touch "$fixture/start-fail"
must_fail check
status_is failed
rm "$fixture/start-fail"
touch "$fixture/busy"
must_fail install
rm "$fixture/busy"

# Linux additionally exercises real cross-process flock ownership while a
# simulated apt job is live. A new status reader must not unlock the worker.
if [ "${LILYGO_TEST_REAL_FLOCK:-0}" = 1 ]; then
    touch "$fixture/install-hold"
    sh "$helper" install
    sh "$helper" worker &
    worker_pid=$!
    attempts=0
    while [ ! -e "$fixture/install-waiting" ]; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 100 ] || { kill "$worker_pid"; exit 1; }
        sleep 0.1
    done
    status_is installing
    must_fail check
    kill -0 "$worker_pid"
    rm "$fixture/install-hold"
    wait "$worker_pid"
    printf 'inactive\n' > "$fixture/service"
    status_is succeeded
fi

# Privilege/path validation happens before any package mutation.
if LILYGO_TEST_UID=1000 sh "$helper" check > "$fixture/output" 2>&1; then exit 1; fi
rm "$job/request"
ln -s "$fixture/service" "$job/request"
must_fail worker
rm "$job/request"
chmod 0777 "$job"
must_fail check
chmod 0700 "$job"
rm "$job/status"
ln -s "$fixture/service" "$job/status"
must_fail status

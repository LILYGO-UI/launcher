#include "process.h"

#include <assert.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define MESA_LIBRARY_PATH "/usr/lib/lilygo-ui/mesa-vc4/lib"
#define MESA_BACKENDS_PATH MESA_LIBRARY_PATH "/gbm"

static int environment_equals(const char *name, const char *expected)
{
    const char *actual = getenv(name);
    return expected ? actual && strcmp(actual, expected) == 0 : actual == NULL;
}

static void set_optional_environment(const char *name, const char *value)
{
    assert((value ? setenv(name, value, 1) : unsetenv(name)) == 0);
}

static int child_mode(void)
{
    const char *home = getenv("LAUNCHER_TEST_HOME");
    const char *name = getenv("LAUNCHER_TEST_NAME");
    const char *shell = getenv("LAUNCHER_TEST_SHELL");
    const char *runtime = getenv("LAUNCHER_TEST_RUNTIME");
    if (!home || !name || !shell || !runtime) return 10;
    if (getuid() != (uid_t)strtoul(getenv("LAUNCHER_TEST_UID"), NULL, 10) ||
        geteuid() != getuid() ||
        getgid() != (gid_t)strtoul(getenv("LAUNCHER_TEST_GID"), NULL, 10) ||
        getegid() != getgid())
        return 11;
    if (!getenv("HOME") || strcmp(getenv("HOME"), home) != 0 || !getenv("USER") ||
        strcmp(getenv("USER"), name) != 0 || !getenv("LOGNAME") || strcmp(getenv("LOGNAME"), name) != 0 ||
        !getenv("SHELL") || strcmp(getenv("SHELL"), shell) != 0 || !getenv("XDG_RUNTIME_DIR") ||
        strcmp(getenv("XDG_RUNTIME_DIR"), runtime) != 0)
        return 12;
    char expected[1024];
    snprintf(expected, sizeof(expected), "%s/.config", home);
    if (!getenv("XDG_CONFIG_HOME") || strcmp(getenv("XDG_CONFIG_HOME"), expected) != 0) return 13;
    snprintf(expected, sizeof(expected), "%s/.local/share", home);
    if (!getenv("XDG_DATA_HOME") || strcmp(getenv("XDG_DATA_HOME"), expected) != 0) return 14;
    snprintf(expected, sizeof(expected), "%s/.cache", home);
    if (!getenv("XDG_CACHE_HOME") || strcmp(getenv("XDG_CACHE_HOME"), expected) != 0) return 15;
    snprintf(expected, sizeof(expected), "%s/.local/state", home);
    if (!getenv("XDG_STATE_HOME") || strcmp(getenv("XDG_STATE_HOME"), expected) != 0) return 16;
    if (getenv("LD_PRELOAD") || getenv("LD_LILYGO_TEST") || getenv("SUDO_UID") || getenv("PYTHONPATH"))
        return 17;
#ifdef __linux__
    const char *expected_nnp = getenv("LAUNCHER_TEST_NO_NEW_PRIVS");
    if (!expected_nnp || prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) != atoi(expected_nnp)) return 18;
    /* Dropping the service identity remains irreversible without an
     * independently authorized setuid helper, even when NNP is unset. */
    if (setuid(0) == 0 || setgid(0) == 0) return 21;
#endif
    if (!environment_equals("LD_LIBRARY_PATH", getenv("LAUNCHER_TEST_LIBRARY_PATH"))) return 19;
    if (!environment_equals("GBM_BACKENDS_PATH", getenv("LAUNCHER_TEST_BACKENDS_PATH"))) return 20;
    return 23;
}

int main(int argc, char **argv)
{
    if (getenv("LAUNCHER_IDENTITY_CHILD")) return child_mode();
    assert(argc == 1);

    const int root_parent = geteuid() == 0;
    const struct passwd *account = root_parent ? getpwnam("nobody") : getpwuid(geteuid());
    assert(account && account->pw_name && account->pw_dir && account->pw_shell);
    launcher_user_t user = {0};
    user.uid = account->pw_uid;
    user.gid = root_parent ? account->pw_gid : getegid();
    snprintf(user.name, sizeof(user.name), "%s", account->pw_name);
    snprintf(user.home, sizeof(user.home), "%s", account->pw_dir);
    snprintf(user.shell, sizeof(user.shell), "%s", account->pw_shell);
    snprintf(user.runtime_dir, sizeof(user.runtime_dir), "/tmp/launcher-runtime-%lu", (unsigned long)user.uid);

    char number[32];
#ifdef __linux__
    const int parent_nnp = prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L);
    assert(parent_nnp == 0 || parent_nnp == 1);
    snprintf(number, sizeof(number), "%d", parent_nnp);
    assert(setenv("LAUNCHER_TEST_NO_NEW_PRIVS", number, 1) == 0);
#endif
    setenv("LAUNCHER_IDENTITY_CHILD", "1", 1);
    setenv("LAUNCHER_TEST_HOME", user.home, 1);
    setenv("LAUNCHER_TEST_NAME", user.name, 1);
    setenv("LAUNCHER_TEST_SHELL", user.shell, 1);
    setenv("LAUNCHER_TEST_RUNTIME", user.runtime_dir, 1);
    snprintf(number, sizeof(number), "%lu", (unsigned long)user.uid);
    setenv("LAUNCHER_TEST_UID", number, 1);
    snprintf(number, sizeof(number), "%lu", (unsigned long)user.gid);
    setenv("LAUNCHER_TEST_GID", number, 1);
    setenv("LD_PRELOAD", "/tmp/launcher-test-preload.so", 1);
    setenv("LD_LILYGO_TEST", "must-not-reach-child", 1);
    setenv("SUDO_UID", "0", 1);
    setenv("PYTHONPATH", "/root/.local/lib", 1);

    launcher_app_t app = {0};
    snprintf(app.executable_path, sizeof(app.executable_path), "%s", argv[0]);
    snprintf(app.manifest.id, sizeof(app.manifest.id), "cc.lilygo.ui.ProcessIdentityTest");
    snprintf(app.manifest.name, sizeof(app.manifest.name), "Process Identity Test");
    launcher_process_options_t options = {
        "sdl", "/dev/fb-test", "/dev/dri/card-test", 42, "/dev/input-test", "landscape", 568, 1232, &user};
    volatile sig_atomic_t stop_requested = 0;
    const struct {
        const char *library_path;
        const char *backends_path;
        const char *expected_library_path;
    } cases[] = {
        {NULL, NULL, NULL},
        {"/tmp/launcher-test-libraries", NULL, NULL},
        {MESA_LIBRARY_PATH, MESA_BACKENDS_PATH, MESA_LIBRARY_PATH},
        {"/tmp/launcher-test-libraries", MESA_BACKENDS_PATH, NULL},
        {MESA_LIBRARY_PATH ":/tmp/launcher-test-libraries", MESA_BACKENDS_PATH, NULL},
        {"/tmp/launcher-test-libraries:" MESA_LIBRARY_PATH, MESA_BACKENDS_PATH, NULL},
        {MESA_LIBRARY_PATH ":", MESA_BACKENDS_PATH, NULL},
        {":" MESA_LIBRARY_PATH, MESA_BACKENDS_PATH, NULL},
        {"", MESA_BACKENDS_PATH, NULL},
        {MESA_LIBRARY_PATH, NULL, NULL},
        {MESA_LIBRARY_PATH, "/tmp/launcher-test-gbm", NULL},
        {MESA_LIBRARY_PATH, MESA_BACKENDS_PATH ":/tmp/launcher-test-gbm", NULL},
    };
    const char *parent_home_value = getenv("HOME");
    char *parent_home = parent_home_value ? strdup(parent_home_value) : NULL;
    assert(!parent_home_value || parent_home);
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        set_optional_environment("LD_LIBRARY_PATH", cases[index].library_path);
        set_optional_environment("GBM_BACKENDS_PATH", cases[index].backends_path);
        set_optional_environment("LAUNCHER_TEST_LIBRARY_PATH", cases[index].expected_library_path);
        set_optional_environment("LAUNCHER_TEST_BACKENDS_PATH", cases[index].backends_path);
        assert(launcher_process_run(&app, &options, &stop_requested) == 23);
        assert(environment_equals("LD_LIBRARY_PATH", cases[index].library_path));
        assert(environment_equals("GBM_BACKENDS_PATH", cases[index].backends_path));
        assert(environment_equals("LD_PRELOAD", "/tmp/launcher-test-preload.so"));
        assert(environment_equals("LD_LILYGO_TEST", "must-not-reach-child"));
        assert(environment_equals("SUDO_UID", "0"));
        assert(environment_equals("PYTHONPATH", "/root/.local/lib"));
        assert(environment_equals("HOME", parent_home));
    }
    free(parent_home);

#ifdef __linux__
    /* A caller may already be restricted. Launcher must preserve that
     * inherited state; Linux never allows clearing it. Isolate this case so
     * the test runner and later tests retain their original privileges. */
    const pid_t restricted = fork();
    assert(restricted >= 0);
    if (restricted == 0) {
        assert(prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) == 0);
        assert(setenv("LAUNCHER_TEST_NO_NEW_PRIVS", "1", 1) == 0);
        _exit(launcher_process_run(&app, &options, &stop_requested) == 23 ? 0 : 1);
    }
    int status = 0;
    assert(waitpid(restricted, &status, 0) == restricted);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(prctl(PR_GET_NO_NEW_PRIVS, 0L, 0L, 0L, 0L) == parent_nnp);
#endif

    launcher_user_t invalid = user;
    if (!root_parent) {
        invalid.uid = user.uid + 1;
        options.user = &invalid;
        assert(launcher_process_run(&app, &options, &stop_requested) == 126);
    }
    invalid = user;
    invalid.uid = 0;
    options.user = &invalid;
    assert(launcher_process_run(&app, &options, &stop_requested) == 126);
    return 0;
}

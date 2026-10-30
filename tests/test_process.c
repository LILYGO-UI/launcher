#include "process.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct poll_state {
    pid_t parent;
    int ready_fd;
    int release_fd;
    int ready;
    int calls;
    int ready_calls;
    long long last_call;
    volatile sig_atomic_t *request_stop;
};

static long long monotonic_ms(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void poll_child(void *data)
{
    struct poll_state *state = data;
    assert(getpid() == state->parent);
    const long long now = monotonic_ms();
    if (state->calls) assert(now - state->last_call >= 450);
    state->last_call = now;
    ++state->calls;
    if (!state->ready) {
        char ready;
        const ssize_t result = read(state->ready_fd, &ready, 1);
        if (result < 0 && errno == EAGAIN) return;
        assert(result == 1 && ready == 'r');
        state->ready = 1;
    }
    ++state->ready_calls;
    if (state->request_stop) {
        *state->request_stop = 1;
    } else if (state->ready_calls == 2) {
        assert(write(state->release_fd, "x", 1) == 1);
    }
}

static void waiting_child(const launcher_app_t *app, const launcher_process_options_t *options,
                          int terminate, int ignore_term)
{
    int release[2], ready[2];
    assert(pipe(release) == 0);
    assert(pipe(ready) == 0);
    assert(fcntl(ready[0], F_SETFL, O_NONBLOCK) == 0);
    char descriptor[32];
    snprintf(descriptor, sizeof(descriptor), "%d", release[0]);
    assert(setenv("LAUNCHER_PROCESS_TEST_WAIT_FD", descriptor, 1) == 0);
    snprintf(descriptor, sizeof(descriptor), "%d", ready[1]);
    assert(setenv("LAUNCHER_PROCESS_TEST_READY_FD", descriptor, 1) == 0);
    if (ignore_term) assert(setenv("LAUNCHER_PROCESS_TEST_IGNORE_TERM", "1", 1) == 0);
    volatile sig_atomic_t stop_requested = 0;
    struct poll_state state = {
        .parent = getpid(),
        .ready_fd = ready[0],
        .release_fd = release[1],
        .request_stop = terminate ? &stop_requested : NULL,
    };
    const int expected = terminate ? 128 + (ignore_term ? SIGKILL : SIGTERM) : 23;
    assert(launcher_process_run_with_poll(app, options, &stop_requested, poll_child, &state) == expected);
    assert(state.ready);
    assert(state.calls >= (terminate ? 1 : 2));
    assert(unsetenv("LAUNCHER_PROCESS_TEST_WAIT_FD") == 0);
    assert(unsetenv("LAUNCHER_PROCESS_TEST_READY_FD") == 0);
    assert(unsetenv("LAUNCHER_PROCESS_TEST_IGNORE_TERM") == 0);
    assert(close(release[0]) == 0);
    assert(close(release[1]) == 0);
    assert(close(ready[0]) == 0);
    assert(close(ready[1]) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    launcher_app_t app = {0};
    assert(snprintf(app.executable_path, sizeof(app.executable_path), "%s", argv[1]) <
           (int)sizeof(app.executable_path));
    snprintf(app.manifest.id, sizeof(app.manifest.id), "cc.lilygo.ui.ProcessTest");
    snprintf(app.manifest.name, sizeof(app.manifest.name), "Process Test");

    launcher_process_options_t options = {
        .display_backend  = "sdl",
        .fbdev_path       = "/dev/fb-test",
        .drm_path         = "/dev/dri/card-test",
        .drm_connector_id = 42,
        .input_path       = "/dev/input-test",
        .orientation      = "landscape",
        .width            = 568,
        .height           = 1232,
    };
    volatile sig_atomic_t stop_requested = 0;
    assert(launcher_process_run(&app, &options, &stop_requested) == 23);
    waiting_child(&app, &options, 0, 0);
    waiting_child(&app, &options, 1, 0);
    waiting_child(&app, &options, 1, 1);
    // The original API remains usable after runs with maintenance callbacks.
    assert(launcher_process_run(&app, &options, &stop_requested) == 23);

    snprintf(app.executable_path, sizeof(app.executable_path), "/missing/app");
    assert(launcher_process_run(&app, &options, &stop_requested) == 127);
    return 0;
}

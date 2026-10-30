#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *expected[] = {
        "--app-id",        "cc.lilygo.ui.ProcessTest",
        "--app-name",      "Process Test",
        "--display",       "sdl",
        "--width",         "568",
        "--height",        "1232",
        "--fbdev",         "/dev/fb-test",
        "--drm",           "/dev/dri/card-test",
        "--drm-connector", "42",
        "--input",         "/dev/input-test",
        "--orientation",   "landscape",
    };
    if (argc != 21) return 1;
    for (int i = 0; i < 20; ++i)
        if (strcmp(argv[i + 1], expected[i]) != 0) return 2;
    const char *wait_descriptor = getenv("LAUNCHER_PROCESS_TEST_WAIT_FD");
    const char *ready_descriptor = getenv("LAUNCHER_PROCESS_TEST_READY_FD");
    if (wait_descriptor && ready_descriptor) {
        if (getenv("LAUNCHER_PROCESS_TEST_IGNORE_TERM")) signal(SIGTERM, SIG_IGN);
        /* Bound a regression failure instead of leaving a hung test child. */
        alarm(10);
        if (write(atoi(ready_descriptor), "r", 1) != 1) return 3;
        char release;
        ssize_t result;
        do {
            result = read(atoi(wait_descriptor), &release, 1);
        } while (result < 0 && errno == EINTR);
        if (result != 1 || release != 'x') return 4;
    }
    return 23;
}

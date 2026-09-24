#include <stdlib.h>
#include <string.h>

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
    return 23;
}

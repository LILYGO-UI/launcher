#ifndef LAUNCHER_USER_H
#define LAUNCHER_USER_H

/*
 * A launcher_user_t is deliberately a value type.  The launcher can keep the
 * selected account in its own state without retaining pointers into passwd(5)
 * storage (or into a login UI buffer).
 */
#include <sys/types.h>

#define LAUNCHER_USER_NAME_MAX 64
#define LAUNCHER_USER_PATH_MAX 1024
#define LAUNCHER_USER_SHELL_MAX 256

typedef struct launcher_user {
    uid_t uid;
    gid_t gid;
    char name[LAUNCHER_USER_NAME_MAX];
    char home[LAUNCHER_USER_PATH_MAX];
    char shell[LAUNCHER_USER_SHELL_MAX];
    char runtime_dir[LAUNCHER_USER_PATH_MAX];
} launcher_user_t;

#endif

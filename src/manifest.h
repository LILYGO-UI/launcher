#ifndef LAUNCHER_MANIFEST_H
#define LAUNCHER_MANIFEST_H

#include <stddef.h>

#define LAUNCHER_ID_MAX 64
#define LAUNCHER_NAME_MAX 96
#define LAUNCHER_DESCRIPTION_MAX 192
#define LAUNCHER_PATH_MAX 1024
#define LAUNCHER_ICON_MAX LAUNCHER_PATH_MAX
#define LAUNCHER_MANIFEST_SCHEMA_VERSION 2

typedef struct launcher_manifest {
    char id[LAUNCHER_ID_MAX];
    char name[LAUNCHER_NAME_MAX];
    char description[LAUNCHER_DESCRIPTION_MAX];
    char icon[LAUNCHER_ICON_MAX];
    char executable[LAUNCHER_PATH_MAX];
    int order;
} launcher_manifest_t;

#ifdef __cplusplus
extern "C" {
#endif

int launcher_manifest_load(const char *path, launcher_manifest_t *manifest,
                           char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

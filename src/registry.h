#ifndef LAUNCHER_REGISTRY_H
#define LAUNCHER_REGISTRY_H

#include "manifest.h"

#include <stddef.h>

#define LAUNCHER_MAX_APPS 128

typedef struct launcher_app {
    launcher_manifest_t manifest;
    char manifest_path[LAUNCHER_PATH_MAX];
    char executable_path[LAUNCHER_PATH_MAX];
    char icon_path[LAUNCHER_PATH_MAX];
} launcher_app_t;

typedef struct launcher_registry {
    launcher_app_t apps[LAUNCHER_MAX_APPS];
    size_t count;
} launcher_registry_t;

#ifdef __cplusplus
extern "C" {
#endif

void launcher_registry_init(launcher_registry_t *registry);
size_t launcher_registry_scan(launcher_registry_t *registry, const char *directory);
size_t launcher_registry_prune_missing(launcher_registry_t *registry);
void launcher_registry_sort(launcher_registry_t *registry);
void launcher_registry_destroy(launcher_registry_t *registry);

#ifdef __cplusplus
}
#endif

#endif

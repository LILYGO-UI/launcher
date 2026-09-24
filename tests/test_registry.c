#include "registry.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_manifest(const char *path, const char *executable, const char *name, int order)
{
    char manifest[LAUNCHER_PATH_MAX + 256];
    int length = snprintf(manifest, sizeof(manifest),
                          "{\"schema_version\":2,\"id\":\"cc.lilygo.ui.RegistryTest\","
                          "\"name\":\"%s\",\"description\":\"Fixture\","
                          "\"icon\":\"icon.png\",\"order\":%d,\"executable\":\"%s\"}",
                          name, order, executable);
    assert(length > 0 && length < (int)sizeof(manifest));
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(manifest, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char directory[] = "/tmp/launcher-registry-XXXXXX";
    assert(mkdtemp(directory));
    char manifest_path[LAUNCHER_PATH_MAX];
    assert(snprintf(manifest_path, sizeof(manifest_path), "%s/app.json", directory) < (int)sizeof(manifest_path));
    char icon_path[LAUNCHER_PATH_MAX];
    assert(snprintf(icon_path, sizeof(icon_path), "%s/icon.png", directory) < (int)sizeof(icon_path));
    write_manifest(manifest_path, argv[1], "Registry Test", 1);
    FILE *icon = fopen(icon_path, "wb");
    assert(icon);
    assert(fputc(0, icon) == 0);
    fclose(icon);

    launcher_registry_t registry;
    launcher_registry_init(&registry);
    assert(launcher_registry_scan(&registry, directory) == 1);
    assert(registry.count == 1);
    assert(strcmp(registry.apps[0].manifest.id, "cc.lilygo.ui.RegistryTest") == 0);
    assert(strcmp(registry.apps[0].icon_path, icon_path) == 0);
    assert(launcher_registry_scan(&registry, directory) == 0);
    assert(registry.count == 1);

    char preferred_directory[] = "/tmp/launcher-registry-preferred-XXXXXX";
    assert(mkdtemp(preferred_directory));
    char preferred_manifest_path[LAUNCHER_PATH_MAX];
    assert(snprintf(preferred_manifest_path, sizeof(preferred_manifest_path), "%s/app.json", preferred_directory) <
           (int)sizeof(preferred_manifest_path));
    char preferred_icon_path[LAUNCHER_PATH_MAX];
    assert(snprintf(preferred_icon_path, sizeof(preferred_icon_path), "%s/icon.png", preferred_directory) <
           (int)sizeof(preferred_icon_path));
    write_manifest(preferred_manifest_path, argv[1], "Preferred Registry Test", 0);
    FILE *preferred_icon = fopen(preferred_icon_path, "wb");
    assert(preferred_icon);
    assert(fputc(0, preferred_icon) == 0);
    fclose(preferred_icon);

    assert(launcher_registry_scan(&registry, preferred_directory) == 0);
    assert(strcmp(registry.apps[0].manifest.name, "Registry Test") == 0);

    launcher_registry_t refreshed;
    launcher_registry_init(&refreshed);
    assert(launcher_registry_scan(&refreshed, preferred_directory) == 1);
    assert(launcher_registry_scan(&refreshed, directory) == 0);
    launcher_registry_sort(&refreshed);
    assert(refreshed.count == 1);
    assert(strcmp(refreshed.apps[0].manifest.name, "Preferred Registry Test") == 0);
    assert(strcmp(refreshed.apps[0].manifest_path, preferred_manifest_path) == 0);
    assert(!launcher_registry_equal(&registry, &refreshed));

    launcher_registry_t duplicate_refresh;
    launcher_registry_init(&duplicate_refresh);
    assert(launcher_registry_scan(&duplicate_refresh, preferred_directory) == 1);
    assert(launcher_registry_scan(&duplicate_refresh, directory) == 0);
    launcher_registry_sort(&duplicate_refresh);
    assert(launcher_registry_equal(&refreshed, &duplicate_refresh));
    duplicate_refresh.apps[0].manifest.order++;
    assert(!launcher_registry_equal(&refreshed, &duplicate_refresh));
    launcher_registry_destroy(&duplicate_refresh);
    launcher_registry_destroy(&refreshed);

    assert(unlink(preferred_icon_path) == 0);
    assert(unlink(preferred_manifest_path) == 0);
    assert(rmdir(preferred_directory) == 0);

    assert(unlink(icon_path) == 0);
    assert(launcher_registry_prune_missing(&registry) == 0);
    assert(registry.count == 1);
    assert(unlink(manifest_path) == 0);
    assert(launcher_registry_prune_missing(&registry) == 1);
    assert(registry.count == 0);
    launcher_registry_destroy(&registry);
    assert(rmdir(directory) == 0);
    return 0;
}

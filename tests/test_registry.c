#include "registry.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_manifest(const char *path, const char *executable)
{
    char manifest[LAUNCHER_PATH_MAX + 256];
    int length = snprintf(
        manifest, sizeof(manifest),
        "{\"schema_version\":2,\"id\":\"cc.lilygo.ui.RegistryTest\","
        "\"name\":\"Registry Test\",\"description\":\"Fixture\","
        "\"icon\":\"icon.png\",\"order\":1,\"executable\":\"%s\"}",
        executable);
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
    assert(snprintf(manifest_path, sizeof(manifest_path), "%s/app.json", directory) <
           (int)sizeof(manifest_path));
    char icon_path[LAUNCHER_PATH_MAX];
    assert(snprintf(icon_path, sizeof(icon_path), "%s/icon.png", directory) <
           (int)sizeof(icon_path));
    write_manifest(manifest_path, argv[1]);
    FILE *icon = fopen(icon_path, "wb");
    assert(icon);
    assert(fputc(0, icon) == 0);
    fclose(icon);

    launcher_registry_t registry;
    launcher_registry_init(&registry);
    assert(launcher_registry_scan(&registry, directory) == 1);
    assert(registry.count == 1);
    assert(strcmp(registry.apps[0].manifest.id,
                  "cc.lilygo.ui.RegistryTest") == 0);
    assert(strcmp(registry.apps[0].icon_path, icon_path) == 0);
    assert(launcher_registry_scan(&registry, directory) == 0);
    assert(registry.count == 1);

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

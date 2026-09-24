#include "manifest.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    fclose(file);
}

int main(void)
{
    char path[]    = "/tmp/launcher-manifest-XXXXXX";
    int descriptor = mkstemp(path);
    assert(descriptor >= 0);
    close(descriptor);
    char error[256];
    launcher_manifest_t manifest;

    write_file(path,
               "{\"schema_version\":2,\"id\":\"cc.lilygo.ui.Test\","
               "\"name\":\"Test App\",\"description\":\"Hello\","
               "\"icon\":\"play\",\"order\":7,\"executable\":\"app\","
               "\"future\":{\"nested\":[true,2]}}");
    assert(launcher_manifest_load(path, &manifest, error, sizeof(error)) == 0);
    assert(strcmp(manifest.id, "cc.lilygo.ui.Test") == 0);
    assert(strcmp(manifest.executable, "app") == 0);
    assert(strcmp(manifest.icon, "play") == 0);
    assert(manifest.order == 7);

    write_file(path,
               "{\"schema_version\":2,\"id\":\"cc.lilygo.ui.IconPath\","
               "\"name\":\"Icon Path\","
               "\"icon\":\"../../icons/hicolor/128x128/apps/cc.lilygo.ui.IconPath.png\","
               "\"executable\":\"app\"}");
    assert(launcher_manifest_load(path, &manifest, error, sizeof(error)) == 0);
    assert(strcmp(manifest.icon, "../../icons/hicolor/128x128/apps/cc.lilygo.ui.IconPath.png") == 0);

    write_file(path, "{\"schema_version\":2,\"id\":\"bad id\",\"name\":\"x\",\"executable\":\"x\"}");
    assert(launcher_manifest_load(path, &manifest, error, sizeof(error)) != 0);
    write_file(path, "{\"schema_version\":2,\"id\":\"ok\",\"name\":\"x\",\"executable\":\"x\",}");
    assert(launcher_manifest_load(path, &manifest, error, sizeof(error)) != 0);
    write_file(path, "{\"schema_version\":2,\"id\":\"ok\",\"name\":\"x\",\"executable\":\"x\",\"bad\":truth}");
    assert(launcher_manifest_load(path, &manifest, error, sizeof(error)) != 0);
    unlink(path);
    return 0;
}

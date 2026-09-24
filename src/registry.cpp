#include "registry.h"
#include "logging.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <memory>

namespace {

struct DirectoryCloser {
    void operator()(DIR *directory) const noexcept
    {
        closedir(directory);
    }
};

}  // namespace

static int has_json_suffix(const char *name)
{
    size_t length = strlen(name);
    return length > 5 && strcmp(name + length - 5, ".json") == 0;
}

static int already_registered(const launcher_registry_t *registry, const char *id)
{
    for (size_t i = 0; i < registry->count; ++i)
        if (strcmp(registry->apps[i].manifest.id, id) == 0) return 1;
    return 0;
}

static int resolve_manifest_path(const char *manifest_path, const char *value, char *output, size_t output_size)
{
    if (value[0] == '/') return snprintf(output, output_size, "%s", value) < (int)output_size ? 0 : -1;
    char directory[LAUNCHER_PATH_MAX];
    if (snprintf(directory, sizeof(directory), "%s", manifest_path) >= (int)sizeof(directory)) return -1;
    char *slash = strrchr(directory, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(directory, sizeof(directory), ".");
    return snprintf(output, output_size, "%s/%s", directory, value) < (int)output_size ? 0 : -1;
}

static int is_icon_path(const char *icon)
{
    return icon[0] == '/' || strchr(icon, '/') != NULL ||
           (strlen(icon) > 4 && strcmp(icon + strlen(icon) - 4, ".png") == 0);
}

static int apps_equal(const launcher_app_t *left, const launcher_app_t *right)
{
    return strcmp(left->manifest.id, right->manifest.id) == 0 &&
           strcmp(left->manifest.name, right->manifest.name) == 0 &&
           strcmp(left->manifest.description, right->manifest.description) == 0 &&
           strcmp(left->manifest.icon, right->manifest.icon) == 0 &&
           strcmp(left->manifest.executable, right->manifest.executable) == 0 &&
           left->manifest.order == right->manifest.order && strcmp(left->manifest_path, right->manifest_path) == 0 &&
           strcmp(left->executable_path, right->executable_path) == 0 && strcmp(left->icon_path, right->icon_path) == 0;
}

void launcher_registry_init(launcher_registry_t *registry)
{
    memset(registry, 0, sizeof(*registry));
}

size_t launcher_registry_scan(launcher_registry_t *registry, const char *directory)
{
    std::unique_ptr<DIR, DirectoryCloser> dir{opendir(directory)};
    if (!dir) {
        if (errno != ENOENT) launcher::logging::error("cannot scan %s: %s", directory, strerror(errno));
        return 0;
    }
    size_t added = 0;
    struct dirent *entry;
    while ((entry = readdir(dir.get())) != NULL && registry->count < LAUNCHER_MAX_APPS) {
        if (entry->d_name[0] == '.' || !has_json_suffix(entry->d_name)) continue;
        char path[LAUNCHER_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >= (int)sizeof(path)) continue;
        launcher_app_t candidate{};
        char error[256];
        if (launcher_manifest_load(path, &candidate.manifest, error, sizeof(error)) != 0) {
            launcher::logging::warn("ignoring %s: %s", path, error);
            continue;
        }
        if (already_registered(registry, candidate.manifest.id)) {
            continue;
        }
        if (resolve_manifest_path(path, candidate.manifest.executable, candidate.executable_path,
                                  sizeof(candidate.executable_path)) != 0) {
            launcher::logging::warn("executable path is too long in %s", path);
            continue;
        }
        struct stat status;
        if (stat(candidate.executable_path, &status) != 0 || !S_ISREG(status.st_mode) ||
            access(candidate.executable_path, X_OK) != 0) {
            launcher::logging::warn("app is not executable: %s", candidate.executable_path);
            continue;
        }
        if (candidate.manifest.icon[0] && is_icon_path(candidate.manifest.icon)) {
            if (resolve_manifest_path(path, candidate.manifest.icon, candidate.icon_path,
                                      sizeof(candidate.icon_path)) != 0) {
                launcher::logging::warn("icon path is too long in %s", path);
                candidate.icon_path[0] = '\0';
            } else if (stat(candidate.icon_path, &status) != 0 || !S_ISREG(status.st_mode) ||
                       access(candidate.icon_path, R_OK) != 0) {
                launcher::logging::warn("app icon is not readable: %s", candidate.icon_path);
                candidate.icon_path[0] = '\0';
            }
        }
        snprintf(candidate.manifest_path, sizeof(candidate.manifest_path), "%s", path);
        registry->apps[registry->count++] = candidate;
        added++;
    }
    return added;
}

size_t launcher_registry_prune_missing(launcher_registry_t *registry)
{
    size_t removed = 0;
    size_t index   = 0;
    while (index < registry->count) {
        launcher_app_t *app = &registry->apps[index];
        struct stat status;
        if (stat(app->manifest_path, &status) == 0 && stat(app->executable_path, &status) == 0 &&
            S_ISREG(status.st_mode) && access(app->executable_path, X_OK) == 0) {
            index++;
            continue;
        }
        memmove(app, app + 1, (registry->count - index - 1) * sizeof(*app));
        registry->count--;
        removed++;
    }
    return removed;
}

void launcher_registry_sort(launcher_registry_t *registry)
{
    std::sort(registry->apps, registry->apps + registry->count,
              [](const launcher_app_t &left, const launcher_app_t &right) {
                  if (left.manifest.order != right.manifest.order) return left.manifest.order < right.manifest.order;
                  int name_order = strcmp(left.manifest.name, right.manifest.name);
                  if (name_order != 0) return name_order < 0;
                  return strcmp(left.manifest.id, right.manifest.id) < 0;
              });
}

int launcher_registry_equal(const launcher_registry_t *left, const launcher_registry_t *right)
{
    if (!left || !right) return left == right;
    if (left->count != right->count) return 0;
    for (size_t i = 0; i < left->count; ++i)
        if (!apps_equal(&left->apps[i], &right->apps[i])) return 0;
    return 1;
}

void launcher_registry_destroy(launcher_registry_t *registry)
{
    memset(registry, 0, sizeof(*registry));
}

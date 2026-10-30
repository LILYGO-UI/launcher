#include "file_share.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

namespace {

struct TempDirectory {
    char path[PATH_MAX] = "/tmp/lilygo-file-share-XXXXXX";
    TempDirectory()
    {
        assert(mkdtemp(path));
        char resolved[PATH_MAX];
        assert(realpath(path, resolved));
        snprintf(path, sizeof(path), "%s", resolved);
    }
    ~TempDirectory() { std::string command = "rm -rf -- '" + std::string(path) + "'"; assert(system(command.c_str()) == 0); }
};

launcher_user_t user_for(const char *home)
{
    launcher_user_t user{};
    if (getuid() == 0) {
        const struct passwd *nobody = getpwnam("nobody");
        assert(nobody && nobody->pw_uid != 0 && nobody->pw_gid != 0);
        user.uid = nobody->pw_uid;
        user.gid = nobody->pw_gid;
        assert(chown(home, user.uid, user.gid) == 0);
        snprintf(user.name, sizeof(user.name), "%s", nobody->pw_name);
    } else {
        user.uid = getuid();
        user.gid = getgid();
        snprintf(user.name, sizeof(user.name), "test-user");
    }
    snprintf(user.home, sizeof(user.home), "%s", home);
    snprintf(user.shell, sizeof(user.shell), "/bin/sh");
    snprintf(user.runtime_dir, sizeof(user.runtime_dir), "/tmp");
    return user;
}

std::string read_file(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void test_filename_validation()
{
    std::string error;
    assert(launcher::file_share::validate_relative_filename("hello.txt", error));
    for (const std::string &value : {std::string(""), std::string("."), std::string(".."), std::string("../x"),
                                     std::string("/tmp/x"), std::string("a/b"), std::string("a\\b"),
                                     std::string("a\x01b", 3)})
        assert(!launcher::file_share::validate_relative_filename(value, error));
}

void test_directory_and_transfer()
{
    TempDirectory fixture;
    launcher_user_t user = user_for(fixture.path);
    std::string path, error;
    if (!launcher::file_share::resolve_download_directory(&user, path, error)) {
        fprintf(stderr, "resolve failed: %s\n", error.c_str());
        assert(false);
    }
    assert(path == std::string(fixture.path) + "/Downloads");
    struct stat metadata{};
    assert(stat(path.c_str(), &metadata) == 0 && S_ISDIR(metadata.st_mode));
    assert((metadata.st_mode & 0777) == 0700);

    assert(chmod(path.c_str(), 0755) == 0);
    assert(launcher::file_share::resolve_download_directory(&user, path, error));
    assert(stat(path.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0700);

    const std::string payload = "hello world";
    const char *digest = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    auto transfer = launcher::file_share::Transfer::begin(&user, "greeting.txt", payload.size(), digest,
                                                            launcher::file_share::ConflictPolicy::Fail, error);
    assert(transfer);
    assert(transfer->write_chunk(payload.data(), 5, error));
    assert(transfer->write_chunk(payload.data() + 5, payload.size() - 5, error));
    std::string final_name;
    assert(transfer->finish(final_name, error));
    assert(final_name == "greeting.txt");
    assert(read_file(path + "/" + final_name) == payload);
    assert(transfer->completed());
}

void test_hash_size_cancel_and_conflicts()
{
    TempDirectory fixture;
    launcher_user_t user = user_for(fixture.path);
    std::string path, error;
    assert(launcher::file_share::resolve_download_directory(&user, path, error));
    const std::string payload = "hello world";
    const char *digest = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";

    auto mismatch = launcher::file_share::Transfer::begin(&user, "bad.txt", payload.size(),
                                                            "0000000000000000000000000000000000000000000000000000000000000000",
                                                            launcher::file_share::ConflictPolicy::Fail, error);
    assert(mismatch && mismatch->write_chunk(payload.data(), payload.size(), error));
    std::string final_name;
    assert(!mismatch->finish(final_name, error));
    assert(access((path + "/bad.txt").c_str(), F_OK) != 0);

    auto too_short = launcher::file_share::Transfer::begin(&user, "short.txt", payload.size() + 1, digest,
                                                             launcher::file_share::ConflictPolicy::Fail, error);
    assert(too_short && too_short->write_chunk(payload.data(), payload.size(), error));
    assert(!too_short->finish(final_name, error));
    assert(access((path + "/short.txt").c_str(), F_OK) != 0);

    auto cancelled = launcher::file_share::Transfer::begin(&user, "cancelled.txt", payload.size(), digest,
                                                             launcher::file_share::ConflictPolicy::Fail, error);
    assert(cancelled && cancelled->write_chunk(payload.data(), payload.size(), error));
    cancelled->cancel();
    assert(!cancelled->finish(final_name, error));
    assert(access((path + "/cancelled.txt").c_str(), F_OK) != 0);

    std::ofstream(path + "/same.txt") << "old";
    auto fail = launcher::file_share::Transfer::begin(&user, "same.txt", payload.size(), digest,
                                                        launcher::file_share::ConflictPolicy::Fail, error);
    assert(fail && fail->write_chunk(payload.data(), payload.size(), error));
    assert(!fail->finish(final_name, error));
    assert(read_file(path + "/same.txt") == "old");

    auto replace = launcher::file_share::Transfer::begin(&user, "same.txt", payload.size(), digest,
                                                           launcher::file_share::ConflictPolicy::Replace, error);
    assert(replace && replace->write_chunk(payload.data(), payload.size(), error));
    assert(replace->finish(final_name, error));
    assert(read_file(path + "/same.txt") == payload);

    auto rename = launcher::file_share::Transfer::begin(&user, "same.txt", payload.size(), digest,
                                                          launcher::file_share::ConflictPolicy::AutoRename, error);
    assert(rename && rename->write_chunk(payload.data(), payload.size(), error));
    assert(rename->finish(final_name, error));
    assert(final_name == "same (1).txt");
    assert(read_file(path + "/" + final_name) == payload);
}

void test_sequence_and_resource_limits()
{
    TempDirectory fixture;
    launcher_user_t user = user_for(fixture.path);
    std::string path, error;
    assert(launcher::file_share::resolve_download_directory(&user, path, error));
    const std::string payload = "hello world";
    const char *digest = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";

    auto ordered = launcher::file_share::Transfer::begin(&user, "ordered.txt", payload.size(), digest,
                                                          launcher::file_share::ConflictPolicy::Fail, error);
    assert(ordered);
    assert(ordered->write_chunk(0, payload.data(), 5, error));
    assert(!ordered->write_chunk(2, payload.data() + 5, payload.size() - 5, error));
    assert(access((path + "/ordered.txt").c_str(), F_OK) != 0);

    auto oversized = launcher::file_share::Transfer::begin(
        &user, "oversized.txt", launcher::file_share::kMaxChunkBytes + 1,
        "5e8f6f4f3f1b5b7f1c3c5a7c2b8d0e3f8f4a4d6e9f1a7f2b8f7c4d2f9b2a1c0d",
        launcher::file_share::ConflictPolicy::Fail, error);
    assert(oversized);
    std::string chunk(launcher::file_share::kMaxChunkBytes + 1, 'x');
    assert(!oversized->write_chunk(chunk.data(), chunk.size(), error));
    assert(access((path + "/oversized.txt").c_str(), F_OK) != 0);

    auto too_large = launcher::file_share::Transfer::begin(
        &user, "too-large.txt", launcher::file_share::kMaxTransferBytes + 1,
        "0000000000000000000000000000000000000000000000000000000000000000",
        launcher::file_share::ConflictPolicy::Fail, error);
    assert(!too_large);
}

void test_auto_rename_long_names()
{
    TempDirectory fixture;
    launcher_user_t user = user_for(fixture.path);
    std::string path, error, final_name;
    assert(launcher::file_share::resolve_download_directory(&user, path, error));
    const std::string payload = "hello world";
    const char *digest = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    const std::string long_extension = "a." + std::string(253, 'e');
    std::ofstream(path + "/" + long_extension) << "old";
    auto extension = launcher::file_share::Transfer::begin(
        &user, long_extension, payload.size(), digest, launcher::file_share::ConflictPolicy::AutoRename, error);
    assert(extension && extension->write_chunk(payload.data(), payload.size(), error));
    assert(extension->finish(final_name, error));
    assert(final_name.size() == 255 && final_name.substr(final_name.size() - 4) == " (1)");
    assert(read_file(path + "/" + final_name) == payload);

    std::string chinese_name;
    for (unsigned index = 0; index < 83; ++index) chinese_name += "\xe4\xb8\xad"; // 中
    chinese_name += ".txt";
    std::ofstream(path + "/" + chinese_name) << "old";
    auto unicode = launcher::file_share::Transfer::begin(
        &user, chinese_name, payload.size(), digest, launcher::file_share::ConflictPolicy::AutoRename, error);
    assert(unicode && unicode->write_chunk(payload.data(), payload.size(), error));
    assert(unicode->finish(final_name, error));
    assert(final_name.size() == 254);
    assert(final_name == chinese_name.substr(0, 246) + " (1).txt");
    assert(read_file(path + "/" + final_name) == payload);
}

void test_symlink_rejected()
{
    TempDirectory fixture;
    const std::string real_downloads = std::string(fixture.path) + "/real-downloads";
    assert(mkdir(real_downloads.c_str(), 0700) == 0);
    const std::string link = std::string(fixture.path) + "/Downloads";
    assert(symlink("real-downloads", link.c_str()) == 0);
    launcher_user_t user = user_for(fixture.path);
    std::string path, error;
    assert(!launcher::file_share::resolve_download_directory(&user, path, error));

    TempDirectory transfer_fixture;
    launcher_user_t transfer_user = user_for(transfer_fixture.path);
    assert(launcher::file_share::resolve_download_directory(&transfer_user, path, error));
    const std::string outside = std::string(transfer_fixture.path) + "/outside.txt";
    std::ofstream(outside) << "outside";
    assert(symlink(outside.c_str(), (path + "/linked.txt").c_str()) == 0);
    const std::string payload = "hello world";
    auto transfer = launcher::file_share::Transfer::begin(
        &transfer_user, "linked.txt", payload.size(),
        "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9",
        launcher::file_share::ConflictPolicy::Replace, error);
    assert(transfer && transfer->write_chunk(payload.data(), payload.size(), error));
    std::string final_name;
    assert(!transfer->finish(final_name, error));
    assert(read_file(outside) == "outside");
}

} // namespace

int main()
{
    test_filename_validation();
    test_directory_and_transfer();
    test_hash_size_cancel_and_conflicts();
    test_sequence_and_resource_limits();
    test_auto_rename_long_names();
    test_symlink_rejected();
    return 0;
}

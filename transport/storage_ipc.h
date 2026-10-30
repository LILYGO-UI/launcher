#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace launcher::transport {

class Storage {
public:
    explicit Storage(int fd);
    ~Storage();
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    std::string request(char command, const std::vector<std::uint8_t> &payload = {});
    void close();

private:
    int fd_;
    std::mutex mutex_;
};

}

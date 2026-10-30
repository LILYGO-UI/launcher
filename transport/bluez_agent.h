#pragma once

#include <memory>

namespace launcher::transport {

class Storage;

class BluezAgent {
public:
    explicit BluezAgent(Storage &storage);
    ~BluezAgent();
    BluezAgent(const BluezAgent &) = delete;
    BluezAgent &operator=(const BluezAgent &) = delete;

    void run();
    void stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}

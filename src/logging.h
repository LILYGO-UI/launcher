#ifndef LAUNCHER_LOGGING_H
#define LAUNCHER_LOGGING_H

namespace launcher::logging {

void info(const char *format, ...) noexcept;
void warn(const char *format, ...) noexcept;
void error(const char *format, ...) noexcept;

class Runtime {
public:
    explicit Runtime(const char *component) noexcept;
    ~Runtime();

    Runtime(const Runtime &)            = delete;
    Runtime &operator=(const Runtime &) = delete;

private:
    const char *previous_component_ = nullptr;
};

}  // namespace launcher::logging

#endif

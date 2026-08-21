#include "logging.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

namespace launcher::logging {

static const char *active_component = "launcher";

static void write_log(const char *level, const char *format,
                      va_list arguments) noexcept {
  time_t now = time(nullptr);
  struct tm local_time {};
  char timestamp[24] = "----/--/-- --:--:--";
  if (localtime_r(&now, &local_time))
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
  fprintf(stderr, "[%s] [%s] [%s] ", timestamp, active_component, level);
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
}

void info(const char *format, ...) noexcept {
  va_list arguments;
  va_start(arguments, format);
  write_log("info", format, arguments);
  va_end(arguments);
}

void warn(const char *format, ...) noexcept {
  va_list arguments;
  va_start(arguments, format);
  write_log("warn", format, arguments);
  va_end(arguments);
}

void error(const char *format, ...) noexcept {
  va_list arguments;
  va_start(arguments, format);
  write_log("error", format, arguments);
  va_end(arguments);
}

Runtime::Runtime(const char *component) noexcept
    : previous_component_(active_component) {
  active_component = component && component[0] ? component : "launcher";
}

Runtime::~Runtime() { active_component = previous_component_; }

} // namespace launcher::logging

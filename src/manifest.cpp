#include "manifest.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <new>

namespace {

struct FileCloser {
  void operator()(FILE *file) const noexcept { fclose(file); }
};

} // namespace

typedef struct json_reader {
  const char *cursor;
  const char *end;
  char *error;
  size_t error_size;
} json_reader_t;

static int fail(json_reader_t *reader, const char *format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(reader->error, reader->error_size, format, args);
  va_end(args);
  return -1;
}

static void skip_space(json_reader_t *reader) {
  while (reader->cursor < reader->end &&
         isspace((unsigned char)*reader->cursor))
    reader->cursor++;
}

static int append_utf8(char *output, size_t capacity, size_t *length,
                       unsigned value) {
  unsigned char bytes[3];
  size_t count;
  if (value <= 0x7fU) {
    bytes[0] = (unsigned char)value;
    count = 1;
  } else if (value <= 0x7ffU) {
    bytes[0] = (unsigned char)(0xc0U | (value >> 6));
    bytes[1] = (unsigned char)(0x80U | (value & 0x3fU));
    count = 2;
  } else {
    bytes[0] = (unsigned char)(0xe0U | (value >> 12));
    bytes[1] = (unsigned char)(0x80U | ((value >> 6) & 0x3fU));
    bytes[2] = (unsigned char)(0x80U | (value & 0x3fU));
    count = 3;
  }
  if (*length + count >= capacity)
    return -1;
  memcpy(output + *length, bytes, count);
  *length += count;
  return 0;
}

static int parse_string(json_reader_t *reader, char *output, size_t capacity) {
  size_t length = 0;
  if (reader->cursor >= reader->end || *reader->cursor++ != '"')
    return fail(reader, "expected a JSON string");

  while (reader->cursor < reader->end) {
    unsigned char ch = (unsigned char)*reader->cursor++;
    if (ch == '"') {
      if (output && length >= capacity)
        return fail(reader, "JSON string is too long");
      if (output)
        output[length] = '\0';
      return 0;
    }
    if (ch < 0x20U)
      return fail(reader, "control character in JSON string");
    if (ch == '\\') {
      if (reader->cursor >= reader->end)
        return fail(reader, "unfinished JSON escape");
      ch = (unsigned char)*reader->cursor++;
      switch (ch) {
      case '"':
      case '\\':
      case '/':
        break;
      case 'b':
        ch = '\b';
        break;
      case 'f':
        ch = '\f';
        break;
      case 'n':
        ch = '\n';
        break;
      case 'r':
        ch = '\r';
        break;
      case 't':
        ch = '\t';
        break;
      case 'u': {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
          if (reader->cursor >= reader->end ||
              !isxdigit((unsigned char)*reader->cursor))
            return fail(reader, "invalid JSON unicode escape");
          unsigned char digit = (unsigned char)*reader->cursor++;
          unsigned nibble = isdigit(digit)
                                ? (unsigned)(digit - (unsigned char)'0')
                                : (unsigned)(tolower(digit) - 'a' + 10);
          value = value * 16U + nibble;
        }
        if (value >= 0xd800U && value <= 0xdfffU)
          return fail(reader, "JSON surrogate pairs are not supported");
        if (output && append_utf8(output, capacity, &length, value) != 0)
          return fail(reader, "JSON string is too long");
        continue;
      }
      default:
        return fail(reader, "invalid JSON escape");
      }
    }
    if (output && length + 1 >= capacity)
      return fail(reader, "JSON string is too long");
    if (output)
      output[length++] = (char)ch;
  }
  return fail(reader, "unterminated JSON string");
}

static int skip_value(json_reader_t *reader, unsigned depth);

static int skip_primitive(json_reader_t *reader) {
  const char *start = reader->cursor;
  while (reader->cursor < reader->end && !strchr(" \t\r\n,]}", *reader->cursor))
    reader->cursor++;
  size_t length = (size_t)(reader->cursor - start);
  if ((length == 4 && memcmp(start, "true", 4) == 0) ||
      (length == 5 && memcmp(start, "false", 5) == 0) ||
      (length == 4 && memcmp(start, "null", 4) == 0))
    return 0;

  const char *cursor = start;
  const char *end = reader->cursor;
  if (cursor < end && *cursor == '-')
    cursor++;
  if (cursor >= end)
    return fail(reader, "invalid JSON primitive");
  if (*cursor == '0') {
    cursor++;
  } else if (*cursor >= '1' && *cursor <= '9') {
    while (cursor < end && isdigit((unsigned char)*cursor))
      cursor++;
  } else {
    return fail(reader, "invalid JSON primitive");
  }
  if (cursor < end && *cursor == '.') {
    cursor++;
    const char *fraction = cursor;
    while (cursor < end && isdigit((unsigned char)*cursor))
      cursor++;
    if (cursor == fraction)
      return fail(reader, "invalid JSON number");
  }
  if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
    cursor++;
    if (cursor < end && (*cursor == '+' || *cursor == '-'))
      cursor++;
    const char *exponent = cursor;
    while (cursor < end && isdigit((unsigned char)*cursor))
      cursor++;
    if (cursor == exponent)
      return fail(reader, "invalid JSON number");
  }
  return cursor == end ? 0 : fail(reader, "invalid JSON primitive");
}

static int skip_compound(json_reader_t *reader, char open, char close,
                         unsigned depth) {
  if (depth > 32U)
    return fail(reader, "JSON nesting is too deep");
  reader->cursor++;
  skip_space(reader);
  if (reader->cursor < reader->end && *reader->cursor == close) {
    reader->cursor++;
    return 0;
  }
  while (reader->cursor < reader->end) {
    if (open == '{') {
      if (parse_string(reader, NULL, 0) != 0)
        return -1;
      skip_space(reader);
      if (reader->cursor >= reader->end || *reader->cursor++ != ':')
        return fail(reader, "expected ':' in JSON object");
    }
    skip_space(reader);
    if (skip_value(reader, depth + 1U) != 0)
      return -1;
    skip_space(reader);
    if (reader->cursor < reader->end && *reader->cursor == close) {
      reader->cursor++;
      return 0;
    }
    if (reader->cursor >= reader->end || *reader->cursor++ != ',')
      return fail(reader, "expected ',' in JSON value");
    skip_space(reader);
  }
  return fail(reader, "unterminated JSON value");
}

static int skip_value(json_reader_t *reader, unsigned depth) {
  skip_space(reader);
  if (reader->cursor >= reader->end)
    return fail(reader, "missing JSON value");
  if (*reader->cursor == '"')
    return parse_string(reader, NULL, 0);
  if (*reader->cursor == '{')
    return skip_compound(reader, '{', '}', depth);
  if (*reader->cursor == '[')
    return skip_compound(reader, '[', ']', depth);
  return skip_primitive(reader);
}

static int parse_integer(json_reader_t *reader, int *value) {
  char *end = NULL;
  errno = 0;
  long parsed = strtol(reader->cursor, &end, 10);
  if (end == reader->cursor || errno != 0 || parsed < INT_MIN ||
      parsed > INT_MAX)
    return fail(reader, "invalid integer");
  reader->cursor = end;
  *value = (int)parsed;
  return 0;
}

static int valid_id(const char *id) {
  if (!id[0])
    return 0;
  for (const unsigned char *cursor = (const unsigned char *)id; *cursor;
       ++cursor)
    if (!isalnum(*cursor) && *cursor != '.' && *cursor != '-' && *cursor != '_')
      return 0;
  return 1;
}

static int parse_manifest(json_reader_t *reader,
                          launcher_manifest_t *manifest) {
  int have_schema = 0, have_id = 0, have_name = 0, have_executable = 0;
  skip_space(reader);
  if (reader->cursor >= reader->end || *reader->cursor++ != '{')
    return fail(reader, "manifest root must be an object");
  skip_space(reader);
  while (reader->cursor < reader->end && *reader->cursor != '}') {
    char key[64];
    if (parse_string(reader, key, sizeof(key)) != 0)
      return -1;
    skip_space(reader);
    if (reader->cursor >= reader->end || *reader->cursor++ != ':')
      return fail(reader, "expected ':' after '%s'", key);
    skip_space(reader);

    if (strcmp(key, "schema_version") == 0) {
      int schema;
      if (parse_integer(reader, &schema) != 0 ||
          schema != LAUNCHER_MANIFEST_SCHEMA_VERSION)
        return fail(reader, "schema_version must be %d",
                    LAUNCHER_MANIFEST_SCHEMA_VERSION);
      have_schema = 1;
    } else if (strcmp(key, "id") == 0) {
      if (parse_string(reader, manifest->id, sizeof(manifest->id)) != 0)
        return -1;
      have_id = 1;
    } else if (strcmp(key, "name") == 0) {
      if (parse_string(reader, manifest->name, sizeof(manifest->name)) != 0)
        return -1;
      have_name = 1;
    } else if (strcmp(key, "description") == 0) {
      if (parse_string(reader, manifest->description,
                       sizeof(manifest->description)) != 0)
        return -1;
    } else if (strcmp(key, "icon") == 0) {
      if (parse_string(reader, manifest->icon, sizeof(manifest->icon)) != 0)
        return -1;
    } else if (strcmp(key, "executable") == 0) {
      if (parse_string(reader, manifest->executable,
                       sizeof(manifest->executable)) != 0)
        return -1;
      have_executable = 1;
    } else if (strcmp(key, "order") == 0) {
      if (parse_integer(reader, &manifest->order) != 0)
        return -1;
    } else if (skip_value(reader, 0) != 0) {
      return -1;
    }
    skip_space(reader);
    if (reader->cursor < reader->end && *reader->cursor == ',') {
      reader->cursor++;
      skip_space(reader);
      if (reader->cursor < reader->end && *reader->cursor == '}')
        return fail(reader, "trailing comma in manifest object");
    } else {
      break;
    }
  }
  if (reader->cursor >= reader->end || *reader->cursor++ != '}')
    return fail(reader, "unterminated manifest object");
  skip_space(reader);
  if (reader->cursor != reader->end)
    return fail(reader, "unexpected data after manifest object");
  if (!have_schema || !have_id || !have_name || !have_executable)
    return fail(reader,
                "schema_version, id, name, and executable are required");
  if (!valid_id(manifest->id))
    return fail(reader,
                "id may only contain letters, digits, '.', '-', and '_'");
  if (!manifest->name[0] || !manifest->executable[0])
    return fail(reader, "name and executable cannot be empty");
  return 0;
}

int launcher_manifest_load(const char *path, launcher_manifest_t *manifest,
                           char *error, size_t error_size) {
  if (!path || !manifest || !error || error_size == 0)
    return -1;
  error[0] = '\0';
  memset(manifest, 0, sizeof(*manifest));

  std::unique_ptr<FILE, FileCloser> file{fopen(path, "rb")};
  if (!file) {
    snprintf(error, error_size, "cannot open %s: %s", path, strerror(errno));
    return -1;
  }
  if (fseek(file.get(), 0, SEEK_END) != 0) {
    snprintf(error, error_size, "cannot seek %s", path);
    return -1;
  }
  long size = ftell(file.get());
  if (size < 0 || size > 65536) {
    snprintf(error, error_size, "manifest size is invalid");
    return -1;
  }
  rewind(file.get());
  std::unique_ptr<char[]> text{
      new (std::nothrow) char[static_cast<size_t>(size) + 1U]};
  if (!text) {
    snprintf(error, error_size, "out of memory");
    return -1;
  }
  size_t read_size =
      fread(text.get(), 1, static_cast<size_t>(size), file.get());
  if (read_size != static_cast<size_t>(size)) {
    snprintf(error, error_size, "cannot read %s", path);
    return -1;
  }
  text[size] = '\0';
  json_reader_t reader{text.get(), text.get() + size, error, error_size};
  return parse_manifest(&reader, manifest);
}

#include "util.hpp"

#include <stdexcept>

#include <string.h>

bool
FormatString(std::string &target, const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  char *buffer;
  int ret = vasprintf(&buffer, format, ap);
  va_end(ap);
  if (ret < 0) {
    return false;
  }
  FreeOnExit freeBuffer(buffer);
  target.assign(buffer);
  return true;
}

bool
AppendFormat(std::string &target, const char *format, ...)
{
  va_list ap;
  va_start(ap, format);
  char *buffer;
  int ret = vasprintf(&buffer, format, ap);
  va_end(ap);
  if (ret < 0) {
    return false;
  }
  FreeOnExit freeBuffer(buffer);
  target += buffer;
  return true;
}

// Magic helpers for dealing with both strerror_r implementations.
namespace {
  bool appendErrorString(std::string &result, char *buf, int ret)
  {
    if (ret == 0) {
      result += buf;
      return true;
    }
    return false;
  }
  bool appendErrorString(std::string &result, char *buf, char *ret)
  {
    if (ret != 0) {
      result =+ buf;
      return true;
    }
    return false;
  }
}

void
AppendErrorString(std::string &result, int code)
{
  char buf[128];
  if (!appendErrorString(result, buf, strerror_r(code, buf, sizeof(buf)))) {
    AppendFormat(result, "Unknown error %d",  code);
  }
}

std::string
ErrorString(int code)
{
  std::string result;
  AppendErrorString(result, code);
  return result;
}

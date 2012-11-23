/*
 * Copyright (C) 2012 Red Hat, Inc.
 * Written by Florian Weimer <fweimer@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "util.hpp"

#include <stdexcept>

#include <string.h>
#include <stdio.h>

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

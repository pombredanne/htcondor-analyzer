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

#pragma once

#include <string>
#include <stdarg.h>
#include <cstdlib>

// Like sprintf, only more secure.
bool FormatString(std::string &target, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));

// Like FormatString, but appends instead of overwriting.
bool AppendFormat(std::string &target, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));

// Appends the errno code string to the first argument.
void AppendErrorString(std::string &, int code);

// Returns the error string for the code.
std::string ErrorString(int code);

// Utility class to invoke free() on a pointer when the scope is left.
class FreeOnExit {
  void *Ptr;
public:
  FreeOnExit(void *ptr) : Ptr(ptr) { }
  ~FreeOnExit() { free(Ptr); }
private:
  FreeOnExit();			// not implemented
  void operator=(const FreeOnExit &); // not implemented
};

#pragma once

#include <string>
#include <stdarg.h>


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
  FreeOnExit() = delete;
  void operator=(const FreeOnExit &) = delete;
};

#include "file.hpp"
#include "util.hpp"

bool
ResolvePath(const char *path, std::string &result)
{
  char *resolved = realpath(path, NULL);
  if (resolved) {
    FreeOnExit freeResolved(resolved);
    result.assign(resolved);
    return true;
  }
  return false;
}


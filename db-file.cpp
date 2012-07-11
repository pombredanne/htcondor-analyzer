#include "db.hpp"
#include "util.hpp"

#include <sys/stat.h>

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

FileIdentification::FileIdentification(const char *path)
  : Mtime(0), Size(0)
{
  if (ResolvePath(path, Path)) {
    struct stat st;
    int ret = lstat(Path.c_str(), &st);
    if (ret == 0) {
      Mtime = st.st_mtime;
      Size = st.st_size;
    } else {
      Path.clear();
    }
  }
}

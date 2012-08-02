#include "db-file.hpp"
#include "db-report.hpp"

#include <stdio.h>

int
main(int argc, char **argv)
{
  Database DB;
  if (argc >= 2) {
    if (!DB.Open(argv[1])) {
      fprintf(stderr, "error: could not open database: %s\n",
	      DB.ErrorMessage.c_str());
      return 1;
    }
  } else {
    if (!DB.Open()) {
      fprintf(stderr, "error: could not open database: %s\n",
	      DB.ErrorMessage.c_str());
      return 1;
    }
  }

  bool ok = Report
    (DB, [](const char *path, unsigned line, unsigned column,
	    const char *tool, const char *message) -> bool {
      printf("%s:%u:%u: (%s) %s\n", path, line, column, tool, message);
      return true;
    });
  return ok ? 0 : 1;
}

#include "db-file.hpp"
#include "db-report.hpp"
#include "LineEditor.hpp"

#include <stdio.h>

int
main(int argc, char **argv)
{
  bool verbose = false;
  int opt;
  while ((opt = getopt(argc, argv, "v")) != -1) {
    switch (opt) {
    case 'v':
      verbose = true;
      break;
    default:
      fprintf(stderr, "usage: %s [-v] [DIRECTORY]\n", argv[0]);
      return 1;
    }
  }

  Database DB;
  if (optind < argc) {
    if (!DB.Open(argv[optind])) {
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
    (DB, [=](const char *path, unsigned line, unsigned column,
	     const char *tool, const char *message) -> bool {
      printf("%s:%u:%u: (%s) %s\n", path, line, column, tool, message);
      if (verbose) {
	LineEditor le;
	le.Read(path);
	std::string text(le.Line(line));
	printf("  %s\n", text.c_str());
	printf("  %s\n", Carets(text, column, 1).c_str());
      }
      return true;
    });
  return ok ? 0 : 1;
}

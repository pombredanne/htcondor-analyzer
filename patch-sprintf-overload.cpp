#include "db.hpp"
#include "db-report.hpp"
#include "LineEditor.hpp"

#include <cstring>
#include <string>
#include <map>

int
main(int argc, char **argv)
{
  int opt;
  bool dry_run = false;
  bool verbose = false;
  while ((opt = getopt(argc, argv, "nv")) != -1) {
    switch (opt) {
    case 'n':
      dry_run = true;
      break;
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

  std::map<std::string, LineEditor> Files;
  bool failed = false;
  bool ok = Report
    (DB, [&](const char *path, unsigned line, unsigned column,
	     const char *tool, const char *message) -> bool {
      if (strcmp(tool, "sprintf-overload") != 0) {
	return true;
      }
      auto &Editor = Files[path];
      if (Editor.LineCount() == 0) {
	if (!Editor.Read(path)) {
	  failed = true;
	  fprintf(stderr, "%s: error: failed to read file\n", path);
	  return false;
	}
      }
      std::string Old(message);
      size_t Pos = Old.find('(');
      if (Pos == std::string::npos) {
	failed = true;
	fprintf(stderr, "%s:%u:%u: could not parse message: %s\n",
		path, line, column, message);
	return false;
      }
      Old.resize(Pos);
      std::string New;
      if (Old == "sprintf") {
	New = "formatstr";
      } else if (Old == "vsprintf") {
	New = "vformatstr";
      } else {
	failed = true;
	fprintf(stderr, "%s:%u:%u: could not parse message: %s\n",
		path, line, column, message);
	return false;
      }
      std::string text;
      if (verbose) {
	text = Editor.Line(line);
      }
      if (!Editor.Patch(line, column, Old,  New)) {
	failed = true;
	fprintf(stderr, "%s:%u:%u: could not apply %s -> %s\n",
		path, line, column, Old.c_str(), New.c_str());
	fprintf(stderr, "  %s\n  ", text.c_str());
	fprintf(stderr, "  %s\n  ", Carets(text, column, Old.size()).c_str());
	return true;
      } else if (verbose) {
	fprintf(stderr, "%s:%u:%u: applying %s -> %s\n",
		path, line, column, Old.c_str(), New.c_str());
	fprintf(stderr, "  %s\n  ", text.c_str());
	fprintf(stderr, "  %s\n  ", Carets(text, column, Old.size()).c_str());
      }
      return true;
    });
  if (ok && !failed) {
    if (!dry_run) {
      for (auto &File : Files) {
	if (!File.second.Write(File.first.c_str())) {
	  fprintf(stderr, "%s: failed to write file\n", File.first.c_str());
	  return 1;
	}
      }
    }
    return 0;
  } else {
    fprintf(stderr, "error: changes not applied because of previous errors\n");
    return 1;
  }
}

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

#include "db.hpp"
#include "db-report.hpp"
#include "LineEditor.hpp"

#include <getopt.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <map>

namespace {

  struct Options {
    bool dry_run;
    bool verbose;

    Options()
      : dry_run(false), verbose(false)
    {
    }
  };

  typedef std::map<std::string, LineEditor> FilesMap;
}

static bool
Callback(const Options &options, bool &failed,
	 FilesMap &Files,
	 const char *path, unsigned line, unsigned column,
	 const char *tool, const char *message)
{
  if (strcmp(tool, "sprintf-overload") != 0) {
    return true;
  }
  
  FilesMap::iterator Editor = Files.find(path);
  if (Editor == Files.end()) {
    if (!Editor->second.Read(path)) {
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
  if (options.verbose) {
    text = Editor->second.Line(line);
  }
  if (!Editor->second.Patch(line, column, Old,  New)) {
    failed = true;
    fprintf(stderr, "%s:%u:%u: could not apply %s -> %s\n",
	    path, line, column, Old.c_str(), New.c_str());
    fprintf(stderr, "  %s\n  ", text.c_str());
    fprintf(stderr, "  %s\n  ", Carets(text, column, Old.size()).c_str());
    return true;
  } else if (options.verbose) {
    fprintf(stderr, "%s:%u:%u: applying %s -> %s\n",
	    path, line, column, Old.c_str(), New.c_str());
    fprintf(stderr, "  %s\n  ", text.c_str());
    fprintf(stderr, "  %s\n  ", Carets(text, column, Old.size()).c_str());
  }
  return true;
}

int
main(int argc, char **argv)
{
  int opt;
  Options options;
  while ((opt = getopt(argc, argv, "nv")) != -1) {
    switch (opt) {
    case 'n':
      options.dry_run = true;
      break;
    case 'v':
      options.verbose = true;
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
  using namespace std::tr1::placeholders;
  bool ok = Report(DB, std::tr1::bind(&Callback, options, failed, Files,
				      _1, _2, _3, _4, _5));
  if (ok && !failed) {
    if (!options.dry_run) {
      for (FilesMap::iterator p = Files.begin(),
	     end = Files.end(); p != end; ++p) {
	if (!p->second.Write(p->first.c_str())) {
	  fprintf(stderr, "%s: failed to write file\n", p->first.c_str());
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

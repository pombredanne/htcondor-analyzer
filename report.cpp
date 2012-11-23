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

#include "db-file.hpp"
#include "db-report.hpp"
#include "LineEditor.hpp"

#include <getopt.h>
#include <stdio.h>

static bool
Callback(bool verbose,
	 const char *path, unsigned line, unsigned column,
	 const char *tool, const char *message)
{
  printf("%s:%u:%u: (%s) %s\n", path, line, column, tool, message);
  if (verbose) {
    LineEditor le;
    le.Read(path);
    std::string text(le.Line(line));
    printf("  %s\n", text.c_str());
    printf("  %s\n", Carets(text, column, 1).c_str());
  }
  return true;
}

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

  using namespace std::tr1::placeholders;
  bool ok = Report(DB, std::tr1::bind(Callback, verbose,
				      _1, _2, _3, _4, _5));
  return ok ? 0 : 1;
}

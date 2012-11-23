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

#include <stdio.h>

int
main()
{
  Database DB;
  if (!DB.Create(Database::FileName)) {
    fprintf(stderr, "could not open database: %s\n", DB.ErrorMessage.c_str());
    return 1;
  }
  if (!DB.Execute
      ("PRAGMA page_size = 4096;"
       "PRAGMA journal_mode = WAL;"

       "CREATE TABLE IF NOT EXISTS files ("
       "id INTEGER PRIMARY KEY, "
       "path TEXT NOT NULL, "
       "mtime INTEGER NOT NULL, "
       "size INTEGER NOT NULL);"
       "CREATE INDEX IF NOT EXISTS files_path ON files (path);"

       "CREATE TABLE IF NOT EXISTS reports ("
       "file INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
       "line INTEGER NOT NULL,"
       "column INTEGER NOT NULL,"
       "tool TEXT NOT NULL,"
       "message TEXT NOT NULL);"
       "CREATE INDEX IF NOT EXISTS reports_file ON reports (file);")) {
    fprintf(stderr, "%s\n", DB.ErrorMessage.c_str());
    return 1;
  }
  return 0;
}

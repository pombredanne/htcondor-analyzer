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

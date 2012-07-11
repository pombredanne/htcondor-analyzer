#include "db.hpp"

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

  // Iterate over all the file names for which we have got anything to
  // report.  For each file, we try to locate the correct internal
  // file ID based for the current file on the disk.
  Statement FileList, FileID, Report;
  if (!(FileList.Prepare(DB, "SELECT DISTINCT path FROM files ORDER BY path")
	&& FileID.Prepare(DB, "SELECT id FROM files "
			  "WHERE path = ? AND mtime = ? AND size = ? "
			  "ORDER BY id DESC LIMIT 1")
	&& Report.Prepare(DB, "SELECT DISTINCT line, column, tool, message "
			  "FROM reports WHERE file = ? ORDER BY rowid"))) {
    fprintf(stderr, "error: %s\n", DB.ErrorMessage.c_str());
    return 1;
  }
  int result = 0;
  while (1) {
    int ret = sqlite3_step(FileList.Ptr);
    if (ret == SQLITE_DONE) {
      break;
    }
    if (ret != SQLITE_ROW) {
      fprintf(stderr, "error: %s\n", DB.ErrorMessage.c_str());
      return 1;
    }
    const char *path = (const char *)sqlite3_column_text(FileList.Ptr, 0);
    FileIdentification FI(path);
    if (!FI.Valid()) {
      fprintf(stderr, "%s: error: could not find file on disk\n", path);
      result = 1;
      continue;
    }
    sqlite3_reset(FileID.Ptr);
    sqlite3_bind_text(FileID.Ptr, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(FileID.Ptr, 2, FI.Mtime);
    sqlite3_bind_int64(FileID.Ptr, 3, FI.Size);
    ret = sqlite3_step(FileID.Ptr);
    if (ret == SQLITE_DONE) {
      fprintf(stderr, "%s: error: could not find report for current file\n",
	      path);
      result = 1;
      continue;
    }
    if (ret != SQLITE_ROW) {
      fprintf(stderr, "error: %s\n", DB.ErrorMessage.c_str());
      return 1;
    }
    sqlite_int64 FID = sqlite3_column_int64(FileID.Ptr, 0);
    sqlite3_reset(Report.Ptr);
    sqlite3_bind_int64(Report.Ptr, 1, FID);
    while (1) {
      ret = sqlite3_step(Report.Ptr);
      if (ret == SQLITE_DONE) {
	break;
      }
      if (ret != SQLITE_ROW) {
	fprintf(stderr, "error: %s\n", DB.ErrorMessage.c_str());
	return 1;
      }
      unsigned line = sqlite3_column_int64(Report.Ptr, 0);
      unsigned column = sqlite3_column_int64(Report.Ptr, 1);
      const char *tool = (const char *)sqlite3_column_text(Report.Ptr, 2);
      const char *message = (const char *)sqlite3_column_text(Report.Ptr, 3);
      printf("%s:%u:%u: (%s) %s\n", path, line, column, tool, message);
    }
  }

  return result;
}

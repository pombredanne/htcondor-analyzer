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
#include "db-file.hpp"
#include "db-report.hpp"

#include <stdio.h>

bool
Report(Database &DB, ReportCallback CB)
{
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
    return false;
  }
  bool result = true;
  while (1) {
    int ret = sqlite3_step(FileList.Ptr);
    if (ret == SQLITE_DONE) {
      break;
    }
    if (ret != SQLITE_ROW) {
      fprintf(stderr, "error: %s\n", DB.ErrorMessage.c_str());
      return false;
    }
    const char *path = (const char *)sqlite3_column_text(FileList.Ptr, 0);
    FileIdentification FI(path);
    if (!FI.Valid()) {
      fprintf(stderr, "%s: error: could not find file on disk\n", path);
      result = false;
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
      result = false;
      continue;
    }
    if (ret != SQLITE_ROW) {
      fprintf(stderr, "error: %s\n", DB.ErrorMessage.c_str());
      return false;
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
	return false;
      }
      unsigned line = sqlite3_column_int64(Report.Ptr, 0);
      unsigned column = sqlite3_column_int64(Report.Ptr, 1);
      const char *tool = (const char *)sqlite3_column_text(Report.Ptr, 2);
      const char *message = (const char *)sqlite3_column_text(Report.Ptr, 3);
      if (!CB(path, line, column, tool, message)) {
	break;
      }
    }
  }
  return result;
}

std::string
Carets(const std::string &Text,
       unsigned Column, unsigned Width)
{
  if (Column == 0) {
    return std::string();
  }
  std::string result;
  result.reserve(Column + Width);
  std::string::size_type end =
    std::min(static_cast<std::string::size_type>(Column - 1), Text.size());
  for (std::string::size_type i = 0; i < end; ++i) {
    char ch = Text.at(i);
    result += ch == '\t' ? '\t' : ' ';
  }
  for (unsigned i = 0; i < Width; ++i) {
    result += '^';
  }
  return result;
}

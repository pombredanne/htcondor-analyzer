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
#include "file.hpp"
#include "util.hpp"

#include <map>
#include <vector>

#include <limits.h>
#include <sys/stat.h>

//////////////////////////////////////////////////////////////////////
// FileIdentification

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

//////////////////////////////////////////////////////////////////////
// FileIdentificationDatabase

struct FileIdentificationDatabase::Impl {
  std::tr1::shared_ptr<Database> DB;

  typedef sqlite_int64 FileID;

  struct FileTableEntry {
    FileIdentification Ident;
    FileID ID;

    FileTableEntry(const std::string &Path)
      : Ident(Path.c_str()), ID(0)
    {
    }
  };

  typedef std::map<std::string, std::tr1::shared_ptr<FileTableEntry> > FTableMap;
  FTableMap FTable;
  typedef std::vector<std::string> TouchedFilesList;
  TouchedFilesList TouchedFiles;

  struct Report {
    std::tr1::shared_ptr<FileTableEntry> FI;
    unsigned Line;
    unsigned Column;
    std::string Tool;
    std::string Message;

    Report(std::tr1::shared_ptr<FileTableEntry> fi,
	   unsigned line,
	   unsigned column,
	   const std::string& tool,
	   const std::string& message)
      : FI(fi),
	Line(line),
	Column(column),
	Tool(tool),
	Message(message)
    {
    }
  };

  std::vector<Report> Reports;

  Impl(std::tr1::shared_ptr<Database> db)
    : DB(db)
  {
  }

  std::tr1::shared_ptr<FileTableEntry> Resolve(const std::string &Path)
  {
    FTableMap::iterator p = FTable.find(Path);
    if (p == FTable.end()) {
      std::tr1::shared_ptr<FileTableEntry> FTE(new FileTableEntry(Path));
      if (FTE->Ident.Valid()) {
	p = FTable.find(FTE->Ident.Path);
	if (p == FTable.end()) {
	  FTable[Path] = FTE;
	  FTable[FTE->Ident.Path] = FTE;
	  return FTE;
	} 
	FTable[Path] = p->second;
      } else {
	// Could not resolve file name.
	DB->ErrorMessage = "could not find file on disk: ";
	DB->ErrorMessage += Path;
	return std::tr1::shared_ptr<FileTableEntry>();
      }
    }
    return p->second;
  }

  bool Record
    (const char *Path, unsigned Line, unsigned Column,
     const char *Tool, const std::string &Message)
  {
    std::tr1::shared_ptr<FileTableEntry> FTE = Resolve(Path);
    if (FTE == NULL) {
      return false;
    }
    Reports.push_back(Report(FTE, Line, Column, Tool, Message));
    return true;
  }

  TransactionResult::Enum RunCommitTransaction()
  {
    TransactionResult::Enum tret;
    for (TouchedFilesList::iterator p = TouchedFiles.begin(),
	   end = TouchedFiles.end(); p != end; ++p) {
      tret = MarkAsProcessed(*p);
      if (tret != TransactionResult::COMMIT) {
	return tret;
      }
    }

    Statement stmt;
    tret = stmt.TxnPrepare
      (*DB, "INSERT INTO files (path, mtime, size) VALUES (?, ?, ?)");
    if (tret != TransactionResult::COMMIT) {
      return tret;
    }
    for (FTableMap::const_iterator p = FTable.begin(),
	   end = FTable.end(); p != end; ++p) {
      const FileIdentification &FI(p->second->Ident);
      if (p->first != FI.Path) {
	// This is a duplicate, secondary entry.  Avoid
	// shadowing the real entry.
	continue;
      }
      sqlite3_reset(stmt.Ptr);
      sqlite3_bind_text(stmt.Ptr, 1, FI.Path.data(), FI.Path.size(),
			SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt.Ptr, 2, FI.Mtime);
      sqlite3_bind_int64(stmt.Ptr, 3, FI.Size);
      if (sqlite3_step(stmt.Ptr) != SQLITE_DONE) {
	return DB->SetTransactionError(sqlite3_sql(stmt.Ptr));
      }
      p->second->ID = sqlite3_last_insert_rowid(DB->Ptr);
    }

    tret = stmt.TxnPrepare
      (*DB,
       "INSERT INTO reports (file, line, column, tool, message) "
       "VALUES (?, ?, ?, ?, ?);");
    if (tret != TransactionResult::COMMIT) {
      return tret;
    }

    for (std::vector<Report>::const_iterator p = Reports.begin(),
	   end = Reports.end(); p != end; ++p) {
      sqlite3_reset(stmt.Ptr);
      sqlite3_bind_int64(stmt.Ptr, 1, p->FI->ID);
      sqlite3_bind_int64(stmt.Ptr, 2, p->Line);
      sqlite3_bind_int64(stmt.Ptr, 3, p->Column);
      sqlite3_bind_text(stmt.Ptr, 4, p->Tool.data(), p->Tool.size(),
			SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt.Ptr, 5, p->Message.data(), p->Message.size(),
			SQLITE_TRANSIENT);
      if (sqlite3_step(stmt.Ptr) != SQLITE_DONE) {
	return DB->SetTransactionError(sqlite3_sql(stmt.Ptr));
      }
    }
    return TransactionResult::COMMIT;
  }

  bool Commit()
  {
    TransactionResult::Enum result = DB->Transact(std::tr1::bind(&Impl::RunCommitTransaction, this));
    return result == TransactionResult::COMMIT;
  }

  TransactionResult::Enum MarkAsProcessed(const std::string &Path)
  {
    std::string Absolute;
    if (!ResolvePath(Path.c_str(), Absolute)) {
      DB->ErrorMessage = "could not find file on disk: ";
      DB->ErrorMessage += Path;
      return TransactionResult::ERROR;
    }

    Statement SQL;
    TransactionResult::Enum tret =
      SQL.TxnPrepare(*DB, "SELECT id FROM files "
		     "WHERE path = ? ORDER BY id DESC LIMIT 1");
    if (tret != TransactionResult::COMMIT) {
      return tret;
    }
    sqlite3_bind_text(SQL.Ptr, 1, Absolute.data(), Absolute.size(),
		      SQLITE_TRANSIENT);
    int ret = sqlite3_step(SQL.Ptr);
    if (ret == SQLITE_DONE) {
      // File is not in the database.  No need to mask older errors.
      return TransactionResult::COMMIT;
    }
    if (ret != SQLITE_ROW) {
      return DB->SetTransactionError(sqlite3_sql(SQL.Ptr));
    }
    // Add an entry for the file, hiding the previous reports.
    // TODO: Only hide changed files? What about plugin changes?
    if (Resolve(Path) == NULL) {
      return TransactionResult::ERROR;
    }
    return TransactionResult::COMMIT;
  }
};

FileIdentificationDatabase::FileIdentificationDatabase(std::tr1::shared_ptr<Database> db)
  : impl(new Impl(db))
{
}

FileIdentificationDatabase::~FileIdentificationDatabase()
{
}

bool FileIdentificationDatabase::isOpen() const
{
  return impl->DB->Ptr != NULL;
}

std::string FileIdentificationDatabase::ErrorMessage() const
{
  return impl->DB->ErrorMessage;
}

bool
FileIdentificationDatabase::Report
  (const char *Path, unsigned Line, unsigned Column,
   const char *Tool, const std::string &Message)
{
  return impl->Record(Path, Line, Column, Tool, Message);
}

void
FileIdentificationDatabase::MarkForProcessing(const char *Path)
{
  impl->TouchedFiles.push_back(Path);
}

bool
FileIdentificationDatabase::Commit()
{
  return impl->Commit();
}


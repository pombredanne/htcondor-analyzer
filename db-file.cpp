#include "db-file.hpp"
#include "file.hpp"
#include "util.hpp"

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

FileIdentificationDatabase::FileIdentificationDatabase(std::shared_ptr<Database> db)
  : DB(std::move(db))
{
}

FileIdentificationDatabase::~FileIdentificationDatabase()
{
}

namespace {
  FileIdentificationDatabase::ID insertIntoDB(FileIdentificationDatabase &DB,
					      const FileIdentification &FI)
  {
    Statement stmt;
    if (!stmt.Prepare
	(*DB.DB, "INSERT INTO files (path, mtime, size) VALUES (?, ?, ?)")) {
      return 0;
    }
    sqlite3_bind_text(stmt.Ptr, 1, FI.Path.data(), FI.Path.size(),
		      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.Ptr, 2, FI.Mtime);
    sqlite3_bind_int64(stmt.Ptr, 3, FI.Size);
    if (stmt.StepRetryOnLocked() != SQLITE_DONE) {
      DB.DB->SetError(sqlite3_sql(stmt.Ptr));
      return 0;
    }
    sqlite3_int64 rowid = sqlite3_last_insert_rowid(DB.DB->Ptr);
    if (rowid <= 0 || rowid > UINT_MAX) {
      DB.DB->ErrorMessage = "row ID outside range";
      return 0;
    }
    return rowid;
  }
}

auto
FileIdentificationDatabase::Resolve(const char *Path) -> ID
{
  std::string PathS(Path);
  auto p = PathToID.find(PathS);
  if (p == PathToID.end()) {
    FileIdentification FI(Path);
    if (FI.Valid()) {
      p = PathToID.find(FI.Path);
      if (p == PathToID.end()) {
	ID id = insertIntoDB(*this, FI);
	if (id != 0) {
	  PathToID[Path] = id;
	  PathToID[FI.Path] = id;
	}
	return id;
      } else {
	PathToID[PathS] = p->second;
      }
    } else {
      // Could not resolve file name.
      DB->ErrorMessage = "could not find file on disk: ";
      DB->ErrorMessage += PathS;
      return 0;
    }
  }
  return p->second;
}


bool
FileIdentificationDatabase::Report
  (FileIdentificationDatabase::ID FID, unsigned Line, unsigned Column,
   const char *Tool, const std::string &Message)
{
  Statement stmt;
  if (!stmt.Prepare
      (*DB,
       "INSERT INTO reports (file, line, column, tool, message) "
       "VALUES (?, ?, ?, ?, ?);")) {
    return false;
  }
  sqlite3_bind_int64(stmt.Ptr, 1, FID);
  sqlite3_bind_int64(stmt.Ptr, 2, Line);
  sqlite3_bind_int64(stmt.Ptr, 3, Column);
  sqlite3_bind_text(stmt.Ptr, 4, Tool, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.Ptr, 5, Message.data(), Message.size(),
		    SQLITE_TRANSIENT);
  if (stmt.StepRetryOnLocked() != SQLITE_DONE) {
    DB->SetError(sqlite3_sql(stmt.Ptr));
    return false;
  }
  return true;
}

bool
FileIdentificationDatabase::MarkForProcessing(const char *Path)
{
  FileIdentification FI(Path);
  if (!FI.Valid()) {
      DB->ErrorMessage = "could not find file on disk: ";
      DB->ErrorMessage += Path;
      return false;
  }
  Statement SQL;
  if (!SQL.Prepare(*DB, "SELECT id FROM files "
		   "WHERE path = ? ORDER BY id DESC LIMIT 1")) {
    return false;
  }
  sqlite3_bind_text(SQL.Ptr, 1, FI.Path.data(), FI.Path.size(),
		    SQLITE_TRANSIENT);
  int ret = SQL.StepRetryOnLocked();
  if (ret == SQLITE_DONE) {
    // File is not in the database.  No need to mask older errors.
    return true;
  }
  if (ret != SQLITE_ROW) {
    return false;
  }
  // Add an entry for the file, hiding the previous reports.
  // TODO: Only hide changed files? What about plugin changes?
  return Resolve(Path) != 0;
}

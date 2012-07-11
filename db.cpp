#include "db.hpp"
#include "util.hpp"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

const char Database::FileName[] = "condor-analyzer.sqlite";

Database::Database()
  : Ptr(nullptr)
{
}

Database::~Database()
{
  sqlite3_close(Ptr);
  Ptr = nullptr;
}

namespace {
  void
  removeTrailingComponent(std::string &path)
  {
    while (!path.empty() && path.at(path.size() - 1) == '/') {
      path.resize(path.size() - 1);
    }
    while (!path.empty() && path.at(path.size() - 1) != '/') {
      path.resize(path.size() - 1);
    }
    while (!path.empty() && path.at(path.size() - 1) == '/') {
      path.resize(path.size() - 1);
    }
  }

  bool
  createOrOpen(Database &DB, const char *path, bool create)
  {
    sqlite3 *db;
    int flags = (create ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_READWRITE;
    int ret = sqlite3_open_v2(path, &db, flags, nullptr);
    if (db == nullptr) {
      DB.ErrorMessage = "out of memory";
      return false;
    }
    if (ret != SQLITE_OK) {
      FormatString(DB.ErrorMessage, "could not open %s: %s",
		   path, sqlite3_errmsg(db));
      sqlite3_close(db);
      return false;
    }
    if (!DB.Close()) {
      sqlite3_close(db);
      return false;
    }
    DB.Ptr = db;
    sqlite3_busy_timeout(DB.Ptr, 15000); // 15 seconds
    return DB.Execute("PRAGMA foreign_keys = ON;");
  }
}

bool
Database::Open(const char *path)
{
  return createOrOpen(*this, path, false);
}

bool
Database::Create(const char *path)
{
  return createOrOpen(*this, path, true);
}

bool
Database::Open()
{
  // getcwd() is not thread-safe, so we do not use it.
  std::string path;
  if (!ResolvePath(".", path)) {
    int code = errno;
    ErrorMessage = "could not resolve current directory: ";
    AppendErrorString(ErrorMessage, code);
    return false;
  }
  std::string pathCopy = path;
  assert(path.at(0) == '/');
  do {
    size_t oldSize = path.size();
    path += '/';
    path += FileName;
    FileIdentification FI(path.c_str());
    if (FI.Valid()) {
      return Open(path.c_str());
    }
    path.resize(oldSize);
    removeTrailingComponent(path);
  } while (path.size() > 1);
  ErrorMessage = "could not find ";
  ErrorMessage += FileName;
  ErrorMessage += " in ";
  ErrorMessage += pathCopy;
  ErrorMessage += " or its parent directories";
  return nullptr;
}

bool
Database::Close()
{
  if (sqlite3_close(Ptr) == SQLITE_OK) {
    Ptr = nullptr;
    return true;
  }
  ErrorMessage = "sqlite3_close: ";
  ErrorMessage += sqlite3_errmsg(Ptr);
  return false;
}

bool
Database::Execute(const char *sql)
{
  while (*sql) {
    const char *tail;
    Statement stmt;
    int ret = sqlite3_prepare_v2(Ptr, sql, -1, &stmt.Ptr, &tail);
    if (ret != SQLITE_OK) {
      SetError("prepare");
      return false;
    }
    if (sqlite3_bind_parameter_count(stmt.Ptr) != 0) {
      ErrorMessage = "trying to execute statement with unbound parameters";
      return false;
    }
    // Consume rows.  PRAGMA produces a row, so we cannot outright
    // rejects such statements.
    do {
      ret = sqlite3_step(stmt.Ptr);
    } while (ret == SQLITE_ROW);
    if (ret != SQLITE_DONE) {
      SetError("step");
      return false;
    }
    sql = tail;
  }
  return true;
}

void
Database::SetError(const char *Context)
{
  ErrorMessage.assign(sqlite3_errmsg(Ptr));
  if (Context) {
    ErrorMessage += " [";
    ErrorMessage += Context;
    ErrorMessage += "]";
  }
}

//////////////////////////////////////////////////////////////////////
// Statement

void
Statement::Close()
{
  sqlite3_finalize(Ptr);
  Ptr = nullptr;
}

bool
Statement::Prepare(Database &DB, const char *sql)
{
  Statement stmt;
  const char *tail;
  if (sqlite3_prepare_v2
      (DB.Ptr, sql, -1, &stmt.Ptr, &tail) != SQLITE_OK) {
    DB.SetError("Prepare");
    return false;
  }
  if (*tail) {
    DB.ErrorMessage = "trailing characters in SQL statement";
    return false;
  }
  swap(stmt);
  return true;
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
    if (sqlite3_step(stmt.Ptr) != SQLITE_DONE) {
      DB.DB->SetError("step");
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
  int ret = sqlite3_step(SQL.Ptr);
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

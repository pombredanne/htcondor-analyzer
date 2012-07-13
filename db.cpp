#include "db.hpp"
#include "file.hpp"
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
    // TODO: this is not effective
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
    if (access(path.c_str(), F_OK) == 0) {
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
      SetError(sqlite3_sql(stmt.Ptr));
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

int
Statement::StepRetryOnLocked()
{
  int ret = sqlite3_step(Ptr);
  if (ret != SQLITE_LOCKED && ret != SQLITE_BUSY) {
    return ret;
  }
  time_t end = time(nullptr) + 15; // retry for 15 seconds
  do {
    // Sleep for 100ms on average.
    usleep((rand() % 100000) + (rand() % 100000));
    ret = sqlite3_step(Ptr);
    if (ret != SQLITE_LOCKED && ret != SQLITE_BUSY) {
      return ret;
    }
  } while (time(nullptr) <= end);
  return ret;
}

#include "db.hpp"
#include "file.hpp"
#include "util.hpp"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

const char Database::FileName[] = "condor-analyzer.sqlite";

static void
RandomSleep(unsigned ms)
{
  // Sleep for ms milliseconds on average.
  usleep((rand() % (ms * 1000)) + (rand() % (ms * 1000)));
}

static inline bool
TemporaryErrorCode(int code)
{
  return code == SQLITE_BUSY || code == SQLITE_LOCKED;
}

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

TransactionResult
Database::SetTransactionError(const char *Context)
{
  SetError(Context);
  if (TemporaryErrorCode(sqlite3_errcode(Ptr))) {
    return TransactionResult::RETRY;
  }
  return TransactionResult::ERROR;
}


TransactionResult
Database::Transact(std::function<TransactionResult()> runner)
{
  Statement stmtBegin, stmtCommit, stmtRollback;
  if (!(stmtBegin.Prepare(*this, "BEGIN")
	&& stmtCommit.Prepare(*this, "COMMIT")
	&& stmtRollback.Prepare(*this, "ROLLBACK"))) {
    return TransactionResult::ERROR;
  }

  auto Rollback = [&]() -> bool {
    if (sqlite3_step(stmtRollback.Ptr) != SQLITE_DONE) {
      SetError("Transact ROLLBACK");
      return false;
    }
    return true;
  };

  const unsigned MaxRetries = 6;
  for (unsigned Retries = 0; Retries < MaxRetries; ++Retries) {
    if (Retries > 0) {
      // Randomized exponential back-off.
      RandomSleep(100 << (Retries - 1));
    }
    int ret = sqlite3_step(stmtBegin.Ptr);
    if (ret != SQLITE_DONE) {
      if (TemporaryErrorCode(ret)) {
	continue;
      }
      SetError("Transact BEGIN");
      return TransactionResult::ERROR;
    }
    TransactionResult result = runner();
    switch (result) {
    case TransactionResult::COMMIT:
      ret = sqlite3_step(stmtCommit.Ptr);
      if (ret != SQLITE_DONE) {
	if (!Rollback()) {
	  return TransactionResult::ERROR;
	}
	continue;
      }
      return TransactionResult::COMMIT;
    case TransactionResult::ROLLBACK:
      if (!Rollback()) {
	return TransactionResult::ERROR;
      }
      return TransactionResult::ROLLBACK;
    case TransactionResult::ERROR:
      sqlite3_step(stmtRollback.Ptr); // preserve original error
      return TransactionResult::ERROR;
    case TransactionResult::RETRY:
      if (!Rollback()) {
	return TransactionResult::ERROR;
      }
      continue;
    default:
      SetError("invalid Transact runner result");
      return TransactionResult::ERROR;
    }
  }
  ErrorMessage = "could not complete Transact";
  return TransactionResult::ERROR;
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
    DB.SetError((std::string("Prepare: ") + sql).c_str());
    return false;
  }
  if (*tail) {
    DB.ErrorMessage = "trailing characters in SQL statement";
    return false;
  }
  swap(stmt);
  return true;
}

TransactionResult
Statement::TxnPrepare(Database &DB, const char *sql)
{
  Statement stmt;
  const char *tail;
  if (sqlite3_prepare_v2
      (DB.Ptr, sql, -1, &stmt.Ptr, &tail) != SQLITE_OK) {
    return DB.SetTransactionError((std::string("Prepare: ") + sql).c_str());
  }
  if (*tail) {
    DB.ErrorMessage = "trailing characters in SQL statement";
    return TransactionResult::ERROR;
  }
  swap(stmt);
  return TransactionResult::COMMIT;
}

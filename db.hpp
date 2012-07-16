#pragma once

#include <algorithm>
#include <string>

#include <sys/types.h>
#include <sqlite3.h>

enum class TransactionResult : int {
  COMMIT,			// Commit the transaction
  ROLLBACK, 			// Roll back the transaction, no retry
  RETRY, 			// Roll back and retry
  ERROR,			// Roll back and report error
};

struct Database {
  static const char FileName[];

  sqlite3 *Ptr;
  std::string ErrorMessage;
  Database();
  ~Database();

  Database(const Database &) = delete;
  void operator=(const Database &) = delete;

  bool Open(const char *Path);
  bool Create(const char *Path);
  bool Open(); // from current directory or its parents
  bool Close();

  bool Execute(const char *);

  // Sets ErrorMessage from the database object.
  void SetError(const char *Context=nullptr);

  // Calls SetError and returns an appropriate transaction result code
  // based on the SQLite error code.
  TransactionResult SetTransactionError(const char *Context=nullptr);

  // Run RUNNER in a transaction.
  TransactionResult Transact(std::function<TransactionResult()> runner);
};

struct Statement {
  sqlite3_stmt *Ptr;
  Statement() : Ptr(nullptr) { }
  Statement(sqlite3_stmt *ptr) : Ptr(ptr) { }
  ~Statement() { sqlite3_finalize(Ptr); }
  Statement(const Statement &) = delete;
  void operator=(const Statement &) = delete;
  void swap(Statement &o) { std::swap(Ptr, o.Ptr); }

  void Close();
  bool Prepare(Database &DB, const char *sql);
};

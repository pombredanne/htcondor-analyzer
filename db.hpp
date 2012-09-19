#pragma once

#include <algorithm>
#include <string>
#include <tr1/functional>

#include <sys/types.h>
#include <sqlite3.h>

struct TransactionResult {
  typedef enum Enum {
    COMMIT,			// Commit the transaction
    ROLLBACK, 			// Roll back the transaction, no retry
    RETRY, 			// Roll back and retry
    ERROR			// Roll back and report error
  } Enum;
private:
  TransactionResult();		// not implemented
  ~TransactionResult();		// not implemented
};

struct Database {
  static const char FileName[];

  sqlite3 *Ptr;
  std::string ErrorMessage;
  Database();
  ~Database();

  bool Open(const char *Path);
  bool Create(const char *Path);
  bool Open(); // from current directory or its parents
  bool Close();

  bool Execute(const char *);

  // Sets ErrorMessage from the database object.
  void SetError(const char *Context=NULL);

  // Calls SetError and returns an appropriate transaction result code
  // based on the SQLite error code.
  TransactionResult::Enum SetTransactionError(const char *Context=NULL);

  // Run RUNNER in a transaction.
  TransactionResult::Enum Transact(std::tr1::function<TransactionResult::Enum()> runner);
private:
  Database(const Database &);	// not implemented
  void operator=(const Database &); // not implemented
};

struct Statement {
  sqlite3_stmt *Ptr;
  Statement() : Ptr(NULL) { }
  Statement(sqlite3_stmt *ptr) : Ptr(ptr) { }
  ~Statement() { sqlite3_finalize(Ptr); }
  void swap(Statement &o) { std::swap(Ptr, o.Ptr); }

  void Close();
  bool Prepare(Database &DB, const char *sql);
  TransactionResult::Enum TxnPrepare(Database &DB, const char *sql);
private:
  Statement(const Statement &);	// not implemented
  void operator=(const Statement &); // not implemented
};

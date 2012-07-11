#pragma once

#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include <sys/types.h>
#include <sqlite3.h>


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
  void SetError();
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

// Determines the canonical name for the path.
bool ResolvePath(const char *path, std::string &result);

struct FileIdentification {
  std::string Path; // canonical path
  time_t Mtime;
  unsigned long long Size;

  // Initializes the object with the data form the specified file.
  FileIdentification(const char *path);
  bool Valid() const { return !Path.empty(); }
};

class FileIdentificationDatabase {
public:
  typedef unsigned ID;
  std::shared_ptr<Database> DB;
  std::map<std::string, unsigned> PathToID;
public:
  FileIdentificationDatabase(std::shared_ptr<Database> db);
  ~FileIdentificationDatabase();

  ID Resolve(const char *Path);

  // Record that the file is subject to processing.  A database entry
  // is added, masking previous reports for the same file.
  bool MarkForProcessing(const char *Path);
};

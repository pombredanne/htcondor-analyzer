#pragma once

#include "db.hpp"

#include <map>
#include <memory>

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
  bool Report(FileIdentificationDatabase::ID FID,
	      unsigned Line, unsigned Column, const char *Tool,
	      const std::string &Message);

  // Record that the file is subject to processing.  A database entry
  // is added, masking previous reports for the same file.
  bool MarkForProcessing(const char *Path);
};

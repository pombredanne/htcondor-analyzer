#pragma once

#include "db.hpp"

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
private:
  struct Impl;
  std::unique_ptr<Impl> impl;
public:
  FileIdentificationDatabase(std::shared_ptr<Database> db);
  ~FileIdentificationDatabase();

  bool isOpen() const;
  std::string ErrorMessage() const;

  bool Report(const char *Path,
	      unsigned Line, unsigned Column, const char *Tool,
	      const std::string &Message);

  // Record that the file is subject to processing.  A database entry
  // is added, masking previous reports for the same file.
  bool MarkForProcessing(const char *Path);
};

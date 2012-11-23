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

#pragma once

#include "db.hpp"

#include <tr1/memory>

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
  std::tr1::shared_ptr<Impl> impl;
public:
  FileIdentificationDatabase(std::tr1::shared_ptr<Database> db);
  ~FileIdentificationDatabase();

  bool isOpen() const;
  std::string ErrorMessage() const;

  bool Report(const char *Path,
	      unsigned Line, unsigned Column, const char *Tool,
	      const std::string &Message);

  // Record that the file is subject to processing.  A database entry
  // is added, masking previous reports for the same file.
  void MarkForProcessing(const char *Path);

  // Write the report to the database.
  bool Commit();
};

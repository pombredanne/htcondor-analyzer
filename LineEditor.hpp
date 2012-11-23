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

#include <tr1/memory>

class LineEditor {
  struct Impl;
  std::tr1::shared_ptr<Impl> impl;
public:
  LineEditor();
  ~LineEditor();

  // Reads the file and stores it in the editor.
  // Discards the previously loaded file and any patches.
  bool Read(const char *Path);

  // Writes the patched file to Path.
  bool Write(const char *Path);

  // Returns the number of lines in the file.  Zero if no file has
  // been read.
  unsigned LineCount() const;

  // Returns the line, or an empty string if it does not exist.
  // Counting starts at one.
  std::string Line(unsigned Number) const;

  // Replaces Old with Null in Line at Column, both one-based.
  // Returns false if the existing text does not match Old.
  bool Patch(unsigned Line, unsigned Column,
	     const std::string &Old, const std::string &New);
};

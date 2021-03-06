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

#include "LineEditor.hpp"

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

struct LineEditor::Impl {
  std::vector<std::string> Lines;
};


LineEditor::LineEditor()
  : impl(new Impl)
{
}

LineEditor::~LineEditor()
{
}

bool
LineEditor::Read(const char *Path)
{
  std::vector<std::string> lines;
  std::string line;
  std::ifstream in(Path);
  if (!in.is_open()) {
    return false;
  }
  for (;;) {
    std::getline(in, line);
    if (in.eof()) {
      break;
    }
    if (!in) {
      return false;
    }
    lines.push_back(line);
  }
  std::swap(lines, impl->Lines);
  return true;
}

bool
LineEditor::Write(const char *Path)
{
  std::string Tmp(Path);
  Tmp += ".new";
  std::ofstream out(Tmp.c_str());
  if (!out.is_open()) {
    return false;
  }
  for (std::vector<std::string>::const_iterator p = impl->Lines.begin(),
	 end = impl->Lines.end(); p != end; ++p) {
    out << *p << '\n';
    if (!out) {
      break;
    }
  }
  out.close();
  if (!out) {
    unlink(Tmp.c_str());
    return false;
  }
  int ret = rename(Tmp.c_str(), Path);
  if (ret < 0) {
    unlink(Tmp.c_str());
    return false;
  }
  return true;
}

unsigned
LineEditor::LineCount() const
{
  return impl->Lines.size();
}

std::string
LineEditor::Line(unsigned Number) const
{
  if (Number == 0 || Number > LineCount()) {
    return std::string();
  }
  return impl->Lines.at(Number - 1);
}

bool
LineEditor::Patch(unsigned Line, unsigned Column,
		  const std::string &Old, const std::string &New)
{
  if (Line == 0 || Line > LineCount()) {
    return false;
  }
  std::string &ToPatch = impl->Lines.at(Line - 1);
  if (Column == 0 || Column > ToPatch.size()) {
    return false;
  }
  unsigned Offset = Column - 1;
  if (Old.size() >= ToPatch.size() - Offset
      || !std::equal(Old.begin(), Old.end(), ToPatch.begin() + Offset)) {
    return false;
  }
  ToPatch.replace(Offset, Old.size(), New);
  return true;
}

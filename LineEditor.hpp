#pragma once

#include <memory>

class LineEditor {
  struct Impl;
  std::unique_ptr<Impl> impl;
public:
  LineEditor();
  LineEditor(LineEditor &&);
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

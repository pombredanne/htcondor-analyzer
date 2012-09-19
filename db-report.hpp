#pragma once

#include <tr1/functional>
#include <string>

class Database;

// Callback function processing report data.
// Called repeatedly as long as the function returns true
// and there is more data.  The string arguments point to temporary
// values owned by the caller.
typedef std::tr1::function<bool(const char *RelativePath,
				unsigned LineNumber, unsigned ColumnNumber,
				const char *ToolName, const char *Message)>
  ReportCallback;


// Run the callback against the database.
bool Report(Database &, ReportCallback);

// Returns carets for the source text, starting at column.  Leading
// characters in the text are replaced with spaces, except for tabs,
// which are left as-is, to preserve indentation.
std::string Carets(const std::string &Text,
		   unsigned Column, unsigned Width);

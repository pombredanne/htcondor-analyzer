#pragma once

#include <functional>

class Database;

// Callback function processing report data.
// Called repeatedly as long as the function returns true
// and there is more data.  The string arguments point to temporary
// values owned by the caller.
typedef std::function<bool(const char *RelativePath,
			   unsigned LineNumber, unsigned ColumnNumber,
			   const char *ToolName, const char *Message)>
  ReportCallback;


// Run the callback against the database.
bool Report(Database &, ReportCallback);

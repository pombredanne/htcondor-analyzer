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

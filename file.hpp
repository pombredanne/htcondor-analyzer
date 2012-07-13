#pragma once

#include <string>

// Determines the canonical name for the path.
bool ResolvePath(const char *path, std::string &result);

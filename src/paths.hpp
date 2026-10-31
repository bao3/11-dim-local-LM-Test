#pragma once

#include <string>

// Directory containing the running binary. Linux: readlink(/proc/self/exe) then dirname.
// Empty when the path cannot be resolved.
std::string executable_dir();

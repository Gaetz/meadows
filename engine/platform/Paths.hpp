#pragma once

#include <filesystem>

namespace platform {

// Directory of the running executable (trailing-slash-free). The anchor for
// locating shipped data; user-writable paths (saves, config) get their own
// function when needed.
std::filesystem::path executableDir();

} // namespace platform

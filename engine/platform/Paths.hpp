#pragma once

#include <ctime>
#include <filesystem>

namespace platform {

// Directory of the running executable (trailing-slash-free). The anchor for
// locating shipped data; user-writable paths (saves, config) get their own
// function when needed.
std::filesystem::path executableDir();

// Portable local time: MSVC's localtime_s does not exist under
// glibc (and C11 Annex K reverses its arguments) — the per-OS branch
// lives in the .cpp, headers stay platform-clean (§3.1).
std::tm localTime(std::time_t time);

} // namespace platform

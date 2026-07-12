#include "engine/platform/Paths.hpp"

#include <SDL3/SDL.h>

namespace platform {

std::filesystem::path executableDir() {
    // SDL caches the result internally; the pointer stays valid.
    const char* base = SDL_GetBasePath();
    return base ? std::filesystem::path { base }
                : std::filesystem::current_path();
}

std::tm localTime(std::time_t time) {
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &time); // MSVC CRT signature
#else
    localtime_r(&time, &local); // POSIX
#endif
    return local;
}

} // namespace platform

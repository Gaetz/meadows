#include "engine/platform/Paths.hpp"

#include <SDL3/SDL.h>

namespace platform {

std::filesystem::path executableDir() {
    // SDL caches the result internally; the pointer stays valid.
    const char* base = SDL_GetBasePath();
    return base ? std::filesystem::path { base }
                : std::filesystem::current_path();
}

} // namespace platform

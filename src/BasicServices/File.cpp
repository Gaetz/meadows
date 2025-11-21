#include "File.h"
#include "Log.h"
#include <SDL3/SDL.h>
#include <fstream>
#include <filesystem>

std::string File::getBasePath() {
    static std::string basePathStr;
    static bool initialized = false;

    if (!initialized) {
        const char* basePath = SDL_GetBasePath();
        if (basePath) {
            basePathStr = std::string(basePath);
            SDL_free((void*)basePath);
        }
        initialized = true;
    }
    return basePathStr;
}

std::vector<char> File::readBinary(const std::string& filepath) {
    std::string fullPath = getBasePath() + filepath;
    
    // Try absolute path first
    std::filesystem::path path = fullPath;
    if (!std::filesystem::exists(path)) {
        Log::Debug("Absolute path not found, trying relative: %s", filepath.c_str());
        // Fallback to relative
        path = filepath;
        if (!std::filesystem::exists(path)) {
            Log::Error("File not found: %s", filepath.c_str());
            return {};  // Return empty vector
        }
    }
    
    // Get file size from filesystem
    std::error_code ec;
    size_t fileSize = std::filesystem::file_size(path, ec);
    if (ec) {
        Log::Error("Failed to get file size for: %s - %s", filepath.c_str(), ec.message().c_str());
        return {};
    }
    
    // Open file
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Log::Error("Failed to open file: %s", filepath.c_str());
        return {};
    }
    
    // Read entire file
    std::vector<char> buffer(fileSize);
    if (!file.read(buffer.data(), static_cast<std::streamsize>(fileSize))) {
        Log::Error("Failed to read file: %s", filepath.c_str());
        return {};
    }
    
    Log::Debug("Successfully loaded file: %s (%zu bytes)", filepath.c_str(), fileSize);
    return buffer;
}

#pragma once

#include <filesystem>

#include "../Defines.h"
#include <vector>

namespace services {

class File {
public:
    // Prevent instantiation
    File() = delete;
    ~File() = delete;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // Path management
    static str getBasePath();
    static std::filesystem::path getFileSystemPath(const str& filepath);

    // Binary file reading
    static std::vector<char> readBinary(const str& filepath);
};

} // namespace services

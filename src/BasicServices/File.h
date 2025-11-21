#pragma once

#include <string>
#include <vector>

class File {
public:
    // Prevent instantiation
    File() = delete;
    ~File() = delete;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // Path management
    static std::string getBasePath();

    // Binary file reading
    static std::vector<char> readBinary(const std::string& filepath);
};

#pragma once

#include <string>
#include <fstream>

class FileWriter {
public:
    FileWriter() = default;
    explicit FileWriter(const std::string& filepath);
    ~FileWriter();
    
    // Disable copy, allow move
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;
    FileWriter(FileWriter&&) = default;
    FileWriter& operator=(FileWriter&&) = default;
    
    bool open(const std::string& filepath);
    void write(const std::string& text);
    void writeLine(const std::string& text);
    bool isOpen() const;
    void close();
    
private:
    std::ofstream stream;
};

#pragma once

#include <string>
#include <fstream>

class FileWriter {
public:
    FileWriter() = default;
    
    void open(const std::string& filepath);
    void write(const std::string& text);
    void writeLine(const std::string& text);
    bool isOpen() const;
    void close();
    
private:
    std::ofstream stream;
};

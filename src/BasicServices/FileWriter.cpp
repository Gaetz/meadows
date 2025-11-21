#include "FileWriter.h"

FileWriter::FileWriter(const std::string& filepath) {
    open(filepath);
}

FileWriter::~FileWriter() {
    close();
}

bool FileWriter::open(const std::string& filepath) {
    close();  // Close any existing file
    stream.open(filepath);
    return stream.is_open();
}

void FileWriter::write(const std::string& text) {
    if (stream.is_open()) {
        stream << text;
    }
}

void FileWriter::writeLine(const std::string& text) {
    if (stream.is_open()) {
        stream << text << "\n";
    }
}

bool FileWriter::isOpen() const {
    return stream.is_open();
}

void FileWriter::close() {
    if (stream.is_open()) {
        stream.close();
    }
}

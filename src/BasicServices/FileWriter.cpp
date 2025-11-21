#include "FileWriter.h"

void FileWriter::open(const std::string& filepath) {
    stream.open(filepath);
}

void FileWriter::write(const std::string& text) {
    if (stream.is_open()) {
        stream << text;
        stream.flush();
    }
}

void FileWriter::writeLine(const std::string& text) {
    if (stream.is_open()) {
        stream << text << "\n";
        stream.flush();
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

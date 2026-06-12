// cooker — text <-> binary plugin tool (§3 on-disk formats).
//
//   cooker cook   <in.toml> <out.bin>    text  -> cooked binary
//   cooker uncook <in.bin>  <out.toml>   cooked binary -> text
//   cooker new-guid [count]              mint authoring guids

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/BinaryFormat.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/core/Log.hpp"

namespace {

int usage() {
    std::printf("usage:\n"
                "  cooker cook   <in.toml> <out.bin>\n"
                "  cooker uncook <in.bin>  <out.toml>\n"
                "  cooker new-guid [count]\n");
    return 2;
}

std::optional<vector<u8>> readFileBytes(const std::filesystem::path& path) {
    std::ifstream file { path, std::ios::binary | std::ios::ate };
    if (!file) {
        return std::nullopt;
    }
    const auto size = file.tellg();
    vector<u8> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file ? std::optional { std::move(bytes) } : std::nullopt;
}

bool writeFileBytes(const std::filesystem::path& path,
                    std::span<const u8> bytes) {
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

int cook(const char* inPath, const char* outPath,
         const data::FormTypeRegistry& types) {
    const auto plugin = data::loadPluginFile(inPath, types);
    if (!plugin) {
        return 1;
    }
    const vector<u8> bytes = data::writePluginBinary(*plugin);
    if (!writeFileBytes(outPath, bytes)) {
        LOG_ERROR("Cannot write {}", outPath);
        return 1;
    }
    LOG_INFO("Cooked {} -> {} ({} records, {} bytes)", inPath, outPath,
             plugin->records.size(), bytes.size());
    return 0;
}

int uncook(const char* inPath, const char* outPath,
           const data::FormTypeRegistry& types) {
    const auto bytes = readFileBytes(inPath);
    if (!bytes) {
        LOG_ERROR("Cannot read {}", inPath);
        return 1;
    }
    const auto plugin = data::readPluginBinary(*bytes, inPath);
    if (!plugin) {
        return 1;
    }
    const str toml = data::writePluginToml(*plugin, types);
    std::ofstream file { outPath, std::ios::trunc };
    file << toml;
    if (!file) {
        LOG_ERROR("Cannot write {}", outPath);
        return 1;
    }
    LOG_INFO("Uncooked {} -> {} ({} records)", inPath, outPath,
             plugin->records.size());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    core::Log::init();

    if (argc < 2) {
        return usage();
    }
    const std::string_view command = argv[1];

    if (command == "new-guid") {
        int count = 1;
        if (argc >= 3) {
            count = std::atoi(argv[2]);
        }
        for (int i = 0; i < count; ++i) {
            std::printf("%s\n", core::Guid::generate().toString().c_str());
        }
        return 0;
    }

    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);

    if (command == "cook" && argc == 4) {
        return cook(argv[2], argv[3], types);
    }
    if (command == "uncook" && argc == 4) {
        return uncook(argv[2], argv[3], types);
    }
    return usage();
}

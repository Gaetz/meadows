#include "engine/render/ShaderLibrary.hpp"

#include <algorithm>

#include <fstream>
#include <sstream>
#include <unordered_set>

#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr f32 kPollInterval = 0.5f; // seconds

// Matches `#include "file"` with optional leading whitespace. Returns the
// quoted path, or empty when the line is not an include.
str parseIncludeLine(const str& line) {
    size_t i = line.find_first_not_of(" \t");
    if (i == str::npos || line.compare(i, 8, "#include") != 0) {
        return {};
    }
    const size_t open = line.find('"', i + 8);
    if (open == str::npos) {
        return {};
    }
    const size_t close = line.find('"', open + 1);
    if (close == str::npos) {
        return {};
    }
    return line.substr(open + 1, close - open - 1);
}

std::filesystem::file_time_type mtimeOf(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type {} : time;
}

// Reads `file`, expanding includes recursively (include-once, so shared
// headers and cycles are safe). Every visited file lands in `files` for the
// hot-reload watch list. Returns false if any file is unreadable.
bool expandFile(const std::filesystem::path& root,
                const std::filesystem::path& file,
                std::unordered_set<str>& includedOnce,
                vector<ShaderLibrary::WatchedFile>& files, str& out);

bool expandStream(const std::filesystem::path& root, std::ifstream& in,
                  std::unordered_set<str>& includedOnce,
                  vector<ShaderLibrary::WatchedFile>& files, str& out) {
    str line;
    while (std::getline(in, line)) {
        const str include = parseIncludeLine(line);
        if (include.empty()) {
            out += line;
            out += '\n';
            continue;
        }
        const std::filesystem::path includePath = root / include;
        const str key = includePath.lexically_normal().generic_string();
        if (includedOnce.contains(key)) {
            continue;
        }
        includedOnce.insert(key);
        if (!expandFile(root, includePath, includedOnce, files, out)) {
            return false;
        }
    }
    return true;
}

bool expandFile(const std::filesystem::path& root,
                const std::filesystem::path& file,
                std::unordered_set<str>& includedOnce,
                vector<ShaderLibrary::WatchedFile>& files, str& out) {
    std::ifstream in { file };
    if (!in) {
        LOG_ERROR("ShaderLibrary: cannot read '{}'", file.string());
        return false;
    }
    files.push_back({ file, mtimeOf(file) });
    return expandStream(root, in, includedOnce, files, out);
}

} // namespace

ShaderLibrary::ShaderLibrary(rhi::Device& device) : device { device } {
#ifdef MEADOWS_SHADER_SOURCE_DIR
    rootDir = MEADOWS_SHADER_SOURCE_DIR;
    if (!std::filesystem::exists(rootDir)) {
        rootDir = platform::executableDir() / "data" / "shaders";
    }
#else
    rootDir = platform::executableDir() / "data" / "shaders";
#endif
    LOG_INFO("ShaderLibrary root: {}", rootDir.string());
}

ShaderLibrary::~ShaderLibrary() {
    for (const rhi::ShaderHandle handle : retired) {
        device.destroyShader(handle);
    }
    for (auto& [name, entry] : entries) {
        device.destroyShader(entry.handle);
    }
}

bool ShaderLibrary::compile(const str& name, Entry& entry) {
    vector<WatchedFile> files;
    str vertexSource;
    str fragmentSource;
    str computeSource;
    bool read = false;
    if (entry.compute) {
        std::unordered_set<str> includedComp;
        read = expandFile(rootDir, rootDir / (name + ".comp"), includedComp,
                          files, computeSource);
    } else {
        std::unordered_set<str> includedVert;
        std::unordered_set<str> includedFrag;
        const str& vertexName =
            entry.vertexName.empty() ? name : entry.vertexName;
        read = expandFile(rootDir, rootDir / (vertexName + ".vert"),
                          includedVert, files, vertexSource) &&
               expandFile(rootDir, rootDir / (name + ".frag"), includedFrag,
                          files, fragmentSource);
    }

    // Refresh the watch list even on failure so a broken state is re-checked
    // only when a file actually changes again (no per-poll retry spam).
    if (!files.empty()) {
        entry.files = std::move(files);
    }
    if (!read) {
        return false;
    }

    const rhi::ShaderHandle handle =
        device.createShader({ .debugName = name,
                              .vertexSource = vertexSource,
                              .fragmentSource = fragmentSource,
                              .computeSource = computeSource,
                              .uniformBlocks = entry.uniformBlocks,
                              .samplers = entry.samplers });
    if (handle.id == 0) {
        return false; // createShader logged the compile/link error
    }
    if (entry.handle.id != 0) {
        // A pipeline may still reference the old program this frame; defer
        // destruction one poll cycle so consumers rebuild first.
        retired.push_back(entry.handle);
    }
    entry.handle = handle;
    return true;
}

rhi::ShaderHandle ShaderLibrary::load(const str& name,
                                      vector<rhi::UniformBlockBinding> uniformBlocks,
                                      vector<rhi::SamplerBinding> samplers,
                                      const str& vertexName) {
    Entry& entry = entries[name];
    entry.vertexName = vertexName;
    entry.uniformBlocks = std::move(uniformBlocks);
    entry.samplers = std::move(samplers);
    if (!compile(name, entry)) {
        LOG_ERROR("ShaderLibrary: initial load of '{}' failed", name);
    }
    return entry.handle;
}

rhi::ShaderHandle ShaderLibrary::loadCompute(
    const str& name, vector<rhi::UniformBlockBinding> uniformBlocks,
    vector<rhi::SamplerBinding> samplers) {
    Entry& entry = entries[name];
    entry.compute = true;
    entry.uniformBlocks = std::move(uniformBlocks);
    entry.samplers = std::move(samplers);
    if (!compile(name, entry)) {
        LOG_ERROR("ShaderLibrary: initial load of compute '{}' failed", name);
    }
    return entry.handle;
}

rhi::ShaderHandle ShaderLibrary::get(const str& name) const {
    if (watching &&
        std::find(watchNames.begin(), watchNames.end(), name) ==
            watchNames.end()) {
        watchNames.push_back(name);
    }
    const auto it = entries.find(name);
    return it != entries.end() ? it->second.handle : rhi::ShaderHandle {};
}

u64 ShaderLibrary::generation(const str& name) const {
    const auto it = entries.find(name);
    return it != entries.end() ? it->second.generation : 0;
}

void ShaderLibrary::pollHotReload(f32 dt) {
    pollTimer += dt;
    if (pollTimer < kPollInterval) {
        return;
    }
    pollTimer = 0.0f;

    // One full cycle has passed: every consumer has seen the new generation
    // and rebuilt its pipeline — the old programs are safe to delete.
    for (const rhi::ShaderHandle handle : retired) {
        device.destroyShader(handle);
    }
    retired.clear();

    for (auto& [name, entry] : entries) {
        bool changed = false;
        for (const WatchedFile& file : entry.files) {
            if (mtimeOf(file.path) != file.mtime) {
                changed = true;
                break;
            }
        }
        if (!changed) {
            continue;
        }
        if (compile(name, entry)) {
            ++entry.generation;
            LOG_INFO("ShaderLibrary: reloaded '{}' (generation {})", name,
                     entry.generation);
        } else {
            LOG_WARN("ShaderLibrary: reload of '{}' failed — keeping the "
                     "previous program",
                     name);
        }
    }
}

} // namespace render

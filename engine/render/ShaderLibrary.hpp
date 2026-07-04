#pragma once

#include <filesystem>
#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class Device;
}

namespace render {

// Loads GLSL shader pairs (<name>.vert + <name>.frag) from the shader root,
// expanding #include "file.glsl" directives (root-relative, include-once).
// Owns the resulting rhi::ShaderHandles.
//
// Hot reload: pollHotReload() checks file timestamps (throttled) and
// recompiles edited shaders — a broken edit keeps the previous program and
// logs the error. Every successful reload bumps the entry's generation;
// consumers compare it each frame and rebuild their pipelines when it moves
// (pipelines are cheap: program + VAO). Retired programs are destroyed one
// poll cycle later, giving consumers a full cycle to drop them.
//
// Root: MEADOWS_SHADER_SOURCE_DIR (Debug builds — editing the repo's shader
// sources reloads live) when it exists, else <exe>/data/shaders.
class ShaderLibrary {
public:
    explicit ShaderLibrary(rhi::Device& device);
    ~ShaderLibrary();

    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;

    // Loads and compiles a shader pair (<name>.vert + <name>.frag). Returns
    // a null handle on failure (missing file, compile error) — the entry is
    // still registered so a later hot reload can fix it. `vertexName`
    // overrides the vertex file (shared fullscreen triangle for post passes).
    rhi::ShaderHandle load(const str& name,
                           vector<rhi::UniformBlockBinding> uniformBlocks = {},
                           vector<rhi::SamplerBinding> samplers = {},
                           const str& vertexName = {});

    rhi::ShaderHandle get(const str& name) const;

    // Starts at 1; bumps on every successful hot reload of `name`.
    u64 generation(const str& name) const;

    // Call once per frame. Internally throttled to ~2 Hz.
    void pollHotReload(f32 dt);

    const std::filesystem::path& root() const { return rootDir; }

    struct WatchedFile {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime {};
    };

private:
    struct Entry {
        rhi::ShaderHandle handle {};
        u64 generation { 1 };
        str vertexName; // empty = same as the entry name
        vector<rhi::UniformBlockBinding> uniformBlocks;
        vector<rhi::SamplerBinding> samplers;
        vector<WatchedFile> files; // .vert + .frag + every include
    };

    // Re-expands and recompiles; on success swaps the handle (old one goes to
    // `retired`) and refreshes the watch list. Returns success.
    bool compile(const str& name, Entry& entry);

    rhi::Device& device;
    std::filesystem::path rootDir;
    std::unordered_map<str, Entry> entries;
    vector<rhi::ShaderHandle> retired; // destroyed on the next poll cycle
    f32 pollTimer { 0.0f };
};

} // namespace render

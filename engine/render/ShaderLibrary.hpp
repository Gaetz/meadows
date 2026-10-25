#pragma once

#include <filesystem>
#include <unordered_map>

#include "engine/core/Assert.hpp"
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
    // Idempotent: loading a name that already holds a live program returns
    // it untouched (two systems may share a shader without one retiring
    // the other's program).
    rhi::ShaderHandle load(const str& name,
                           vector<rhi::UniformBlockBinding> uniformBlocks = {},
                           vector<rhi::SamplerBinding> samplers = {},
                           const str& vertexName = {});

    // Loads a compute shader (<name>.comp) — same include expansion, hot
    // reload and generation tracking as the graphics pairs.
    rhi::ShaderHandle loadCompute(
        const str& name, vector<rhi::UniformBlockBinding> uniformBlocks = {},
        vector<rhi::SamplerBinding> samplers = {});

    rhi::ShaderHandle get(const str& name) const;

    // Starts at 1; bumps on every successful hot reload of `name`.
    u64 generation(const str& name) const;

    // Hot-reload watch: records the shader names a build pass ACTUALLY
    // consumed (every get() between begin and end), and the generation
    // sum that gates its rebuild. Deriving the list from use kills the
    // drifted-hand-list bug class — a shader feeding a pipeline but
    // missing from a refresh list meant a silently dead hot reload.
    // The bracket is owned by render::WatchedPipeline (the only consumer):
    // an EMPTY watch never fires (sum 0 == 0), which is why the first
    // build is gated there on the handle, not on the watch.
    struct Watch {
        vector<str> names;
        u64 sum { 0 };
        // True when a watched generation moved; re-arms the sum.
        bool changed(const ShaderLibrary& shaders) {
            u64 now = 0;
            for (const str& name : names) {
                now += shaders.generation(name);
            }
            if (now == sum) {
                return false;
            }
            sum = now;
            return true;
        }
    };
    // Call once per frame. Internally throttled to ~2 Hz.
    void pollHotReload(f32 dt);

    const std::filesystem::path& root() const { return rootDir; }

    struct WatchedFile {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime {};
    };

private:
    friend class WatchedPipeline;
    // The watch bracket wraps the BUILD (the get() calls), not the loads:
    // create() loads, the recipe consumes. Nesting is a bug (the inner
    // bracket would erase the outer list) and asserts.
    void beginWatch() const {
        ENGINE_ASSERT_MSG(!watching, "nested shader watch");
        watching = true;
        watchNames.clear();
    }
    Watch endWatch() const {
        ENGINE_ASSERT_MSG(watching, "endWatch without beginWatch");
        watching = false;
        Watch watch;
        watch.names = watchNames;
        for (const str& name : watch.names) {
            watch.sum += generation(name);
        }
        return watch;
    }

    // Watch recording (mutable: get() is const, recording is pure
    // observability).
    mutable bool watching { false };
    mutable vector<str> watchNames;

    struct Entry {
        rhi::ShaderHandle handle {};
        u64 generation { 1 };
        bool compute { false }; // <name>.comp instead of .vert/.frag
        str vertexName; // empty = same as the entry name
        vector<rhi::UniformBlockBinding> uniformBlocks;
        vector<rhi::SamplerBinding> samplers;
        vector<WatchedFile> files; // .vert + .frag + every include
    };

    // Re-expands and recompiles; on success swaps the handle (old one goes to
    // `retired`) and refreshes the watch list. Returns success.
    bool compile(const str& name, Entry& entry);
    // load()/loadCompute() are idempotent on a live entry: a repeated load
    // keeps the first program (and warns when the bindings differ).
    bool alreadyLoaded(const str& name, const Entry& entry,
                       const vector<rhi::UniformBlockBinding>& uniformBlocks,
                       const vector<rhi::SamplerBinding>& samplers) const;

    rhi::Device& device;
    std::filesystem::path rootDir;
    std::unordered_map<str, Entry> entries;
    vector<rhi::ShaderHandle> retired; // destroyed on the next poll cycle
    f32 pollTimer { 0.0f };
};

} // namespace render

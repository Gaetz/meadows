#pragma once

#include "engine/core/Assert.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace render {

// A pipeline that owns its hot-reload bracket: ensure() builds it the
// first time and rebuilds it whenever a shader its recipe consumed was
// reloaded. The recipe (a callable returning the fresh handle, calling
// shaders.get() inside) is the ONLY place the shader names appear, so a
// pipeline can never drift out of its refresh list — the bug class the
// hand-held Watch/generation counters kept reintroducing (a pipeline
// built once in create() and never refreshed, a watch opened inside the
// wrong `if`). A recipe that consumes no shader is a bug and asserts.
//
// Rebuild gates: first use, a consumed shader's generation moved, or
// invalidate() (a recipe input changed — wireframe, a mode). The old
// pipeline is destroyed on the spot, like every manual rebuild before.
class WatchedPipeline {
public:
    // Returns true when the pipeline was (re)built this call.
    template <typename Build>
    bool ensure(rhi::Device& device, const ShaderLibrary& shaders,
                Build&& build) {
        if (handle.id() != 0 && !dirty && !watch.changed(shaders)) {
            return false;
        }
        shaders.beginWatch();
        handle = { device, build() };
        watch = shaders.endWatch();
        ENGINE_ASSERT_MSG(!watch.names.empty(),
                          "pipeline recipe consumed no shader");
        dirty = false;
        return true;
    }

    // Rebuild on the next ensure() even though no shader moved.
    void invalidate() { dirty = true; }
    void reset() {
        handle.reset();
        watch = {};
        dirty = false;
    }

    rhi::PipelineHandle get() const { return handle.get(); }
    operator rhi::PipelineHandle() const { return handle.get(); }
    u32 id() const { return handle.id(); }
    explicit operator bool() const { return handle.id() != 0; }

private:
    rhi::UniquePipeline handle;
    ShaderLibrary::Watch watch;
    bool dirty { false };
};

} // namespace render

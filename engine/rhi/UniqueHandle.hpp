#pragma once

#include "engine/rhi/Rhi.hpp"

namespace rhi {

class Device;

namespace detail {
// One overload per handle type — the .cpp forwards to the Device's
// destroyX. Keeping the dispatch out of the template keeps this header
// free of Device.hpp.
void destroyHandle(Device& device, BufferHandle handle);
void destroyHandle(Device& device, TextureHandle handle);
void destroyHandle(Device& device, SamplerHandle handle);
void destroyHandle(Device& device, ShaderHandle handle);
void destroyHandle(Device& device, PipelineHandle handle);
void destroyHandle(Device& device, BindGroupHandle handle);
void destroyHandle(Device& device, FramebufferHandle handle);
} // namespace detail

// Owning GPU handle: destroys through its device on scope
// exit, reset() or re-assignment — the manual destroy mirrors (onExit
// lists, mark/sweep frees) become impossible to get wrong. Move-only;
// implicit conversion keeps read sites (`setBindGroup(0, group)`,
// `updateBuffer(ubo, …)`) untouched. §8 RAII, and the deliberate small
// step before a full Handle<Tag> unification.
//
// Lifetime note: the wrapper must be reset/destroyed while its Device is
// alive (same contract as MeshCache/TextureCache dtors) — owners that
// outlive scene teardown reset explicitly in their destroy path.
template <typename H>
class Unique {
public:
    Unique() = default;
    Unique(Device& device, H handle) : device_ { &device }, handle_ { handle } {}
    ~Unique() { reset(); }
    Unique(const Unique&) = delete;
    Unique& operator=(const Unique&) = delete;
    Unique(Unique&& other) noexcept
        : device_ { other.device_ }, handle_ { other.handle_ } {
        other.device_ = nullptr;
        other.handle_ = {};
    }
    Unique& operator=(Unique&& other) noexcept {
        if (this != &other) {
            reset();
            device_ = other.device_;
            handle_ = other.handle_;
            other.device_ = nullptr;
            other.handle_ = {};
        }
        return *this;
    }

    void reset() {
        if (device_ != nullptr && handle_.id != 0) {
            detail::destroyHandle(*device_, handle_);
        }
        device_ = nullptr;
        handle_ = {};
    }

    H get() const { return handle_; }
    operator H() const { return handle_; } // read sites stay untouched
    u32 id() const { return handle_.id; }
    explicit operator bool() const { return handle_.id != 0; }

private:
    Device* device_ { nullptr };
    H handle_ {};
};

using UniqueBuffer = Unique<BufferHandle>;
using UniqueTexture = Unique<TextureHandle>;
using UniqueSampler = Unique<SamplerHandle>;
using UniquePipeline = Unique<PipelineHandle>;
using UniqueBindGroup = Unique<BindGroupHandle>;
using UniqueFramebuffer = Unique<FramebufferHandle>;

} // namespace rhi

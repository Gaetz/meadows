#pragma once

// Backend-internal header: everything the Vulkan translation units share
// (resource records, the device state behind VulkanDevice's pimpl, the
// command buffer, the barrier and format helpers). Vulkan, VMA and the
// loader types stay confined to engine/rhi/backends/vulkan/ (§3.1) — no
// file outside this directory may include it.
//
// VMA is header-only: every TU sees the declarations through this header
// with ONE configuration; VulkanDevice.cpp alone defines VMA_IMPLEMENTATION.

#include "engine/rhi/backends/vulkan/VulkanDevice.hpp"

#include <array>
#include <string>
#include <unordered_map>

// Portability-subset structs (MoltenVK feature opt-in) live in vulkan_beta.h.
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#include <vulkan/vulkan.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "engine/core/Log.hpp"

namespace rhi {

// Frames recorded ahead of the GPU. Per-frame resources (command buffer,
// acquire semaphore, fence) are duplicated this many times so the CPU never
// waits on the frame it is recording. The Phase-5 snapshot seam already passes
// render data by value, so deeper pipelining stays a scheduling choice.
inline constexpr u32 kFramesInFlight = 2;

// Timestamp slots reserved per frame-in-flight. The GPU-PERF HUD samples a
// handful of scopes per frame; overflowing warns rather than corrupting.
inline constexpr u32 kTimestampsPerFrame = 64;

inline bool vkOk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        LOG_ERROR("Vulkan: {} failed (VkResult {})", what,
                  static_cast<i32>(result));
        return false;
    }
    return true;
}

// --- Format / state conversion ----------------------------------------------

VkFormat toVkFormat(TextureFormat format);
VkFilter toVkFilter(FilterMode filter);
VkSamplerAddressMode toVkAddressMode(AddressMode mode);
VkCompareOp toVkCompareOp(CompareFunc compare);
VkBufferUsageFlags toVkBufferUsage(BufferUsage usage);

// A barrier's stage masks must be supported by the queue family the
// command buffer belongs to: the upload queue (transfer-only) and the
// async-compute queue (no graphics) reject the shader stages layoutMasks
// derives. Stages the family lacks are dropped; an empty mask becomes
// ALL_COMMANDS (always legal, strictly conservative). Recording them
// anyway was undefined behaviour — the validation layer flagged it on
// every upload, and the driver's intermittent crash lived there.
inline constexpr VkQueueFlags kAllQueueCaps =
    VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;

VkPipelineStageFlags clampStagesToQueue(VkPipelineStageFlags stages,
                                        VkQueueFlags caps);

// Records the layout transition of `mipCount` mips x `layerCount` layers,
// with access/stage masks derived from both layouts and clamped to what
// the recording queue family can execute.
void transitionLayout(VkCommandBuffer cb, VkImage image,
                      VkImageAspectFlags aspect, u32 baseMip, u32 mipCount,
                      u32 layerCount, VkImageLayout oldLayout,
                      VkImageLayout newLayout,
                      VkQueueFlags queueCaps = kAllQueueCaps);

// --- Resource records --------------------------------------------------------

struct VulkanBuffer {
    VkBuffer buffer { VK_NULL_HANDLE };
    VmaAllocation allocation { nullptr };
    u64 size { 0 };
    // Scopes the in-frame update barriers (recordHostUpdate) to the
    // stages that can actually read this buffer.
    BufferUsage usage { BufferUsage::Vertex };
    // Persistently mapped when the buffer lives in host-visible memory
    // (BufferDesc::dynamic or ::readback); nullptr means device-local, which
    // is written/read through a staging copy instead.
    void* mapped { nullptr };
};

struct VulkanTexture {
    VkImage image { VK_NULL_HANDLE };
    VmaAllocation allocation { nullptr };
    VkImageView view { VK_NULL_HANDLE };
    VkFormat format { VK_FORMAT_UNDEFINED };
    VkExtent3D extent {};
    u32 mipLevels { 1 };
    u32 arrayLayers { 1 };
    VkImageAspectFlags aspect { VK_IMAGE_ASPECT_COLOR_BIT };
    // Created with pixelsIncludeMips: the offline chain is authoritative,
    // generateMipmaps refuses to overwrite it.
    bool offlineMips { false };
    // Tracked so transitions know where they are coming from. One layout for
    // the whole image: this backend never leaves mips in mixed layouts outside
    // of generateMipmaps, which restores a uniform one before returning.
    VkImageLayout layout { VK_IMAGE_LAYOUT_UNDEFINED };
    // Built from TextureDesc::filter/wrap at creation. Used when a bind-group
    // entry carries a texture WITHOUT a sampler — the RHI's legacy 2D
    // contract ("the texture's own creation-time parameters apply"), which GL
    // gets for free from glTexParameter and Vulkan has to spell out.
    VkSampler defaultSampler { VK_NULL_HANDLE };
    // Per-mip views, created lazily for storage-image bindings: a storage
    // descriptor must view EXACTLY ONE mip (GL's glBindImageTexture(level)).
    // Binding the whole-texture view silently made every hiz_down pass write
    // mip 0 — mips 1..N stayed garbage and the Hi-Z cull ate distant chunks.
    vector<VkImageView> mipViews;
};

// --- Descriptor binding remap --------------------------------------------------
//
// OpenGL gives UBOs, texture units, SSBOs and image units SEPARATE binding
// namespaces, and the shader corpus relies on it: `binding = 0` is legitimately
// both a UBO and a sampler in 11 shaders. Vulkan has ONE namespace per
// descriptor set, so those declarations collide.
//
// Rather than edit the corpus (which would break the GL backend, where
// `set=` is not even valid syntax), the Vulkan backend shifts each descriptor
// type into its own range while translating the source. The same offsets are
// applied when a BindGroupDesc is turned into a descriptor set, so callers
// keep using the GL-style numbers and never see this.
//
// Ranges are 16 wide; the corpus currently peaks at UBO 5, sampler 11, SSBO 3,
// image 0.
enum class DescriptorClass { Uniform, Sampler, Storage, StorageImage };

inline constexpr u32 kBindingRange = 16;

inline u32 bindingOffset(DescriptorClass klass) {
    switch (klass) {
    case DescriptorClass::Uniform:      return 0 * kBindingRange;
    case DescriptorClass::Sampler:      return 1 * kBindingRange;
    case DescriptorClass::Storage:      return 2 * kBindingRange;
    case DescriptorClass::StorageImage: return 3 * kBindingRange;
    }
    return 0;
}

inline VkDescriptorType toVkDescriptorType(DescriptorClass klass) {
    switch (klass) {
    case DescriptorClass::Uniform: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case DescriptorClass::Sampler:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case DescriptorClass::Storage: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case DescriptorClass::StorageImage:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

// One resource a shader declares, discovered while remapping its bindings.
// This doubles as the reflection createPipeline needs to build its descriptor
// set layout — parsing the GLSL we already rewrite is cheaper (and one fewer
// dependency) than reflecting the SPIR-V afterwards.
// Sampled-image dimensionality, from the GLSL type name. Binding NUMBERS are
// reused with different meanings across shaders (GL semantics), so a group
// replayed onto another pipeline may pair a 3D texture with a 2D binding —
// pushGroup skips those writes (sampling them was undefined in GL too).
enum class ImageDim { Any, T2D, T2DArray, T3D, Cube };

struct ShaderResource {
    u32 binding { 0 }; // already offset
    DescriptorClass klass { DescriptorClass::Uniform };
    // sampler*Shadow: needs the device's immutable PCF sampler (MoltenVK
    // exposes no mutableComparisonSamplers, so a comparison sampler cannot
    // be pushed as a normal descriptor).
    bool comparison { false };
    ImageDim dim { ImageDim::Any };
};

// A linked program in RHI terms: either a vertex+fragment pair or a lone
// compute stage. Vulkan has no "program" object — the modules are combined at
// pipeline creation — so this owns the modules plus the reflection.
struct VulkanShader {
    VkShaderModule vertex { VK_NULL_HANDLE };
    VkShaderModule fragment { VK_NULL_HANDLE };
    VkShaderModule compute { VK_NULL_HANDLE };
    vector<ShaderResource> resources;
};

// An offscreen target. With dynamic rendering there is no VkFramebuffer: this
// only owns the per-subresource image views (a FramebufferAttachment selects a
// mip and a layer) and remembers the formats a pipeline must be built against.
struct VulkanFramebuffer {
    vector<VkImageView> colorViews;
    vector<VkFormat> colorFormats;
    vector<TextureHandle> colorTextures;
    VkImageView depthView { VK_NULL_HANDLE };
    VkFormat depthFormat { VK_FORMAT_UNDEFINED };
    TextureHandle depthTexture {};
    VkExtent2D extent {};
};

// PipelineDesc says nothing about the render target, but a VkPipeline must be
// built against concrete attachment formats. So the state is kept here and the
// VkPipeline is created lazily on first use with a given target, then cached.
struct VulkanPipeline {
    PipelineDesc desc;
    bool compute { false };
    // Bit i = the shader declares binding i (remapped bindings top out at 63:
    // 4 descriptor classes x kBindingRange). setBindGroup filters against
    // this — shared bind groups legitimately carry MORE entries than a given
    // pipeline uses (frame group vs the caster pipelines), which GL ignores
    // and a Vulkan push descriptor would reject.
    u64 bindingMask { 0 };
    // Bindings whose layout carries the immutable PCF sampler: the pushed
    // write's sampler is ignored there, so pushGroup substitutes a plain one
    // (a pushed COMPARISON sampler trips the portability validation).
    u64 comparisonMask { 0 };
    vector<ShaderResource> resources; // for push-time dim checks
    VkDescriptorSetLayout setLayout { VK_NULL_HANDLE };
    VkPipelineLayout layout { VK_NULL_HANDLE };
    std::unordered_map<u64, VkPipeline> variants; // key = target formats
    VkPipeline computePipeline { VK_NULL_HANDLE };
};

// Push descriptors mean a bind group never owns a VkDescriptorSet: it is just
// the list of writes, replayed against whatever pipeline layout is bound.
struct VulkanBindGroup {
    vector<BindGroupEntry> entries;
};

// Identifies a render target by its attachment formats, which is exactly what
// pipeline compatibility depends on under dynamic rendering.
u64 targetKey(const vector<VkFormat>& colorFormats, VkFormat depthFormat);

class VulkanCommandBuffer;

// --- Device state -------------------------------------------------------------

// Validation message tally, filled by the debug-utils callback. File-
// scope type so the C callback can name it (Impl is a private nested type).
struct ValidationCounters {
    u32 errors { 0 };
    u32 warnings { 0 };
};

struct VulkanDevice::Impl {
    platform::Window* window { nullptr };

    VkInstance instance { VK_NULL_HANDLE };
    VkDebugUtilsMessengerEXT debugMessenger { VK_NULL_HANDLE };
    ValidationCounters validationCounts;
    VkSurfaceKHR surface { VK_NULL_HANDLE };
    VkPhysicalDevice gpu { VK_NULL_HANDLE };
    VkDevice device { VK_NULL_HANDLE };
    VmaAllocator allocator { nullptr };

    u32 graphicsFamily { 0 };
    u32 presentFamily { 0 };
    VkQueue graphicsQueue { VK_NULL_HANDLE };
    VkQueue presentQueue { VK_NULL_HANDLE };

    // Async compute (docs/RENDERING.md PG3): a compute-capable family
    // DISTINCT from graphics (its own hardware queue — on MoltenVK a
    // separate MTLCommandQueue, which Apple GPUs schedule concurrently).
    // The chain: graphics N signals the timeline -> compute N waits it,
    // runs, signals -> graphics N+1 waits that value at its consumer
    // stages only. CB reuse is guarded CPU-side by waiting the slot's
    // last signaled value (the graphics fence does not cover compute).
    bool asyncComputeAvailable { false };
    u32 computeFamily { 0 };
    // What the async-compute / upload families can execute (barrier
    // stage masks are clamped to them — clampStagesToQueue).
    VkQueueFlags computeQueueFlags { kAllQueueCaps };
    VkQueueFlags uploadQueueFlags { kAllQueueCaps };
    VkQueue computeQueue { VK_NULL_HANDLE };
    VkCommandPool computePool { VK_NULL_HANDLE };
    std::array<VkCommandBuffer, kFramesInFlight> computeCbs {};
    std::array<u64, kFramesInFlight> computeSlotValue {};
    uptr<VulkanCommandBuffer> computeCmd;
    bool computeRecording { false };
    // The staged-update ROUTING window: only between asyncComputeCmd()
    // and endAsyncCompute() do updateBuffer copies land on the compute
    // CB — a later UI vertex update must stay on the graphics stream
    // (sync-validation caught the cross-queue read).
    bool computeRouting { false };
    VkSemaphore timeline { VK_NULL_HANDLE };
    u64 timelineNext { 0 };      // last value handed out
    u64 computeDoneValue { 0 };  // what the next graphics submit waits

    // Swapchain + everything sized by it (recreated together on resize).
    VkSwapchainKHR swapchain { VK_NULL_HANDLE };
    VkFormat colorFormat { VK_FORMAT_UNDEFINED };
    VkExtent2D extent {};
    vector<VkImage> images;
    vector<VkImageView> imageViews;
    // Signalled when the frame targeting this IMAGE is done; present waits on
    // it. Per-image (not per-frame) so a semaphore is never reused while a
    // previous present is still pending on it.
    vector<VkSemaphore> renderFinished;

    VkCommandPool commandPool { VK_NULL_HANDLE };

    // Extension entry points (loaded in create()).
    PFN_vkCmdBeginRenderingKHR cmdBeginRendering { nullptr };
    PFN_vkCmdEndRenderingKHR cmdEndRendering { nullptr };
    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSet { nullptr };
    // Separate transient pool for the blocking upload/readback submits, so
    // staging work never resets a frame's command buffer.
    VkCommandPool transferPool { VK_NULL_HANDLE };

    std::array<VkCommandBuffer, kFramesInFlight> commandBuffers {};
    std::array<VkSemaphore, kFramesInFlight> imageAvailable {};
    std::array<VkFence, kFramesInFlight> inFlight {};

    u32 frame { 0 };            // frame-in-flight slot being recorded
    u32 imageIndex { 0 };       // swapchain image acquired this frame
    bool frameActive { false }; // false when acquire failed -> endFrame skips
    u64 frameCounter { 0 };     // absolute, for the deferred-free queue

    // Mini deletion queue ('vkDeviceWaitIdle in destroy*' is not
    // enough): destroying mid-RECORDING is unsafe even after an idle —
    // the commands referencing the resource are not submitted yet. Parked
    // here and freed once the frame slot cycles (fence-proven done).
    struct PendingTexture {
        VkImage image; VmaAllocation alloc; VkImageView view;
        VkSampler sampler; vector<VkImageView> mipViews; u64 frame;
    };
    struct PendingBuffer {
        VkBuffer buffer; VmaAllocation alloc; u64 frame;
    };
    struct PendingCmd {
        VkCommandBuffer cb; u64 frame;
    };
    struct PendingSampler {
        VkSampler sampler; u64 frame;
    };
    struct PendingTarget {
        vector<VkImageView> colorViews; VkImageView depthView; u64 frame;
    };
    struct PendingShader {
        VkShaderModule vertex; VkShaderModule fragment; VkShaderModule compute;
        u64 frame;
    };
    struct PendingPipeline {
        vector<VkPipeline> variants; VkPipeline compute;
        VkPipelineLayout layout; VkDescriptorSetLayout setLayout; u64 frame;
    };
    vector<PendingCmd> pendingCmds; // async transfer cbs (see immediateSubmit)
    vector<PendingTexture> pendingTextures;
    vector<PendingBuffer> pendingBuffers;
    vector<PendingSampler> pendingSamplers;
    vector<PendingTarget> pendingTargets;
    vector<PendingShader> pendingShaders;
    vector<PendingPipeline> pendingPipelines;

    // Persistent-mapped staging ring, one per frame slot: in-frame buffer
    // updates carve linear slices out of it instead of paying a VMA
    // allocation (and a deletion-queue entry) per update. The slot's reset
    // in beginFrame happens-after its fence + compute-timeline wait, so
    // the GPU is provably done reading it. Growth parks the old buffer in
    // the deletion queue — copies already recorded keep referencing it.
    struct StagingRing {
        VkBuffer buffer { VK_NULL_HANDLE };
        VmaAllocation allocation { nullptr };
        u8* mapped { nullptr };
        u64 capacity { 0 };
        u64 head { 0 };
    };
    std::array<StagingRing, kFramesInFlight> stagingRings {};
    bool ringAlloc(u64 size, VkBuffer& outBuffer, u64& outOffset,
                   void** outMapped);

    // Dedicated upload (transfer) queue: in-frame DEVICE-LOCAL buffer
    // uploads record into a per-slot transfer command buffer, submitted at
    // endFrame BEFORE the graphics submit. On PC the transfer-only family
    // is a real DMA engine; on MoltenVK it is a third generic queue (the
    // sync topology is identical, so M1 validates the PC path).
    // Ordering: the upload submit WAITS last frame's graphics value (WAR —
    // the previous frame may still read the buffer being rewritten) and
    // signals its OWN timeline (uploadTimeline: one signaler, values stay
    // monotonic — interleaving a third queue on the shared timeline could
    // signal out of order, which the spec forbids); the graphics submit
    // waits that value at the stages that read uploaded data. The compute
    // chain does NOT wait it: the rc chain reads no streamed buffer (extend
    // the wait if that ever changes).
    u32 transferFamily { 0 };
    VkQueue transferQueue { VK_NULL_HANDLE };
    VkCommandPool uploadPool { VK_NULL_HANDLE };
    std::array<VkCommandBuffer, kFramesInFlight> uploadCbs {};
    VkSemaphore uploadTimeline { VK_NULL_HANDLE };
    bool uploadQueueAvailable { false };
    bool uploadRecording { false };
    u64 uploadNext { 0 };
    u64 uploadDoneValue { 0 };
    std::array<u64, kFramesInFlight> uploadSlotValue {};
    u64 gfxDoneValue { 0 }; // last graphics timeline value (upload WAR wait)
    bool recordImageUpload(VkImage image, VkImageAspectFlags aspect,
                           u32 mipLevels, u32 arrayLayers, const void* data,
                           u64 size, vector<VkBufferImageCopy> regions);
    bool recordUpload(VkBuffer dst, const void* data, u64 size,
                      u64 dstOffset);
    // The frame a resource consumed by an UNFENCED async submit is parked
    // under. Inside a frame, this frame's fence (submitted after it on
    // the same queue) covers it. BETWEEN frames the last fence was
    // already submitted: only the NEXT frame's fence orders behind it,
    // so the park moves one frame later — freeing under the previous
    // one raced the copy (the layer's "buffer in use" at every scene
    // load, and a driver crash at exit).
    u64 asyncParkFrame() const {
        return frameActive ? frameCounter : frameCounter + 1;
    }

    bool multiDrawIndirect { false }; // device feature, mirrored in caps
    bool textureCompressionBC { false }; // device feature, mirrored in caps

    void flushPendingFrees(bool force) {
        const auto done = [&](u64 parked) {
            return force || frameCounter >= parked + kFramesInFlight;
        };
        std::erase_if(pendingTextures, [&](const PendingTexture& t) {
            if (!done(t.frame)) { return false; }
            for (VkImageView v : t.mipViews) {
                if (v != VK_NULL_HANDLE) {
                    vkDestroyImageView(device, v, nullptr);
                }
            }
            vkDestroyImageView(device, t.view, nullptr);
            if (t.sampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, t.sampler, nullptr);
            }
            vmaDestroyImage(allocator, t.image, t.alloc);
            return true;
        });
        std::erase_if(pendingBuffers, [&](const PendingBuffer& b) {
            if (!done(b.frame)) { return false; }
            vmaDestroyBuffer(allocator, b.buffer, b.alloc);
            return true;
        });
        std::erase_if(pendingCmds, [&](const PendingCmd& c) {
            if (!done(c.frame)) { return false; }
            vkFreeCommandBuffers(device, transferPool, 1, &c.cb);
            return true;
        });
        std::erase_if(pendingSamplers, [&](const PendingSampler& p) {
            if (!done(p.frame)) { return false; }
            vkDestroySampler(device, p.sampler, nullptr);
            return true;
        });
        std::erase_if(pendingTargets, [&](const PendingTarget& t) {
            if (!done(t.frame)) { return false; }
            for (VkImageView view : t.colorViews) {
                vkDestroyImageView(device, view, nullptr);
            }
            if (t.depthView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, t.depthView, nullptr);
            }
            return true;
        });
        std::erase_if(pendingShaders, [&](const PendingShader& sh) {
            if (!done(sh.frame)) { return false; }
            vkDestroyShaderModule(device, sh.vertex, nullptr);
            vkDestroyShaderModule(device, sh.fragment, nullptr);
            vkDestroyShaderModule(device, sh.compute, nullptr);
            return true;
        });
        std::erase_if(pendingPipelines, [&](const PendingPipeline& p) {
            if (!done(p.frame)) { return false; }
            for (VkPipeline variant : p.variants) {
                vkDestroyPipeline(device, variant, nullptr);
            }
            if (p.compute != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, p.compute, nullptr);
            }
            vkDestroyPipelineLayout(device, p.layout, nullptr);
            vkDestroyDescriptorSetLayout(device, p.setLayout, nullptr);
            return true;
        });
    }

    // Handle tables. Ids start at 1 so 0 stays the invalid handle (§ Rhi.hpp).
    u32 nextId { 1 };
    std::unordered_map<u32, VulkanBuffer> buffers;
    std::unordered_map<u32, VulkanTexture> textures;
    std::unordered_map<u32, VkSampler> samplers;
    // Device-owned comparison sampler baked into layouts as IMMUTABLE for
    // every sampler*Shadow binding (linear + LESS_OR_EQUAL: matches the
    // ShadowMapper's and the key light's PCF samplers).
    VkSampler pcfSampler { VK_NULL_HANDLE };
    // Disk-persisted pipeline cache: MoltenVK compiles the Metal shader at
    // FIRST pipeline use (10-100 ms stalls). Variants are created lazily per
    // target-format set, so a fast camera pan during the first minutes walks
    // through never-seen combinations — visible hitches that self-heal once
    // everything compiled. Persisting the cache moves that cost to the first
    // run only.
    VkPipelineCache pipelineCache { VK_NULL_HANDLE };
    static constexpr const char* kPipelineCacheFile =
        "vulkan-pipeline-cache.bin";
    // GL keeps SOMETHING bound on every unit; these stand in whenever a
    // group entry cannot legally be pushed (declared-dim mismatch, texture
    // busy as an attachment, or destroyed) so no descriptor stays unset.
    TextureHandle dummy2D {};
    TextureHandle dummyArray {};
    TextureHandle dummy3D {};
    TextureHandle dummyDepth {}; // comparison bindings need a depth format
    BufferHandle dummyUniform {};
    BufferHandle dummyStorage {};
    std::unordered_map<u32, VulkanShader> shaders;
    // GPU markers. A fence is signalled by an empty submit, which the spec
    // says completes only after everything already submitted — exactly the
    // "marker after all prior work" the RHI promises.
    std::unordered_map<u32, VkFence> fences;

    // Timestamps live in a per-frame region of one pool: vkCmdResetQueryPool
    // is illegal inside a render pass, so a region can only be reset at
    // beginFrame — by which point its results are two frames old and either
    // polled or abandoned.
    VkQueryPool queryPool { VK_NULL_HANDLE };
    f32 timestampPeriod { 1.0f }; // nanoseconds per tick
    u32 timestampCursor { 0 };    // next slot within the current region
    // Bumped when a region is reset, so a handle from an older generation is
    // recognised as stale instead of reading someone else's result.
    std::array<u32, kFramesInFlight> regionGeneration {};
    // Second pool for the ASYNC COMPUTE queue: a timestamp inserted while
    // computeRouting is on must land in the COMPUTE command buffer — in the
    // graphics stream it would bracket nothing (the rc* blindness). Its
    // per-slot regions are reset at the top of the compute cb (same-queue
    // ordering; a cross-queue reset would race the writes).
    VkQueryPool computeQueryPool { VK_NULL_HANDLE };
    u32 computeTimestampCursor { 0 };
    std::array<u32, kFramesInFlight> computeRegionGeneration {};
    struct PendingTimestamp {
        u32 query { 0 };
        u32 frameSlot { 0 };
        u32 generation { 0 };
        bool onComputePool { false };
        // Harvested at slot-recycle time (see beginFrame): once the region
        // resets, the pool result is gone, but the fence wait that precedes
        // the reset proves the GPU finished — so the value is read into here
        // first and timestampReady can still deliver it.
        u64 cachedTicks { 0 };
        bool cached { false };
    };
    std::unordered_map<u32, PendingTimestamp> timestamps;

    std::unordered_map<u32, VulkanFramebuffer> targets;
    std::unordered_map<u32, VulkanPipeline> pipelines;
    std::unordered_map<u32, VulkanBindGroup> bindGroups;

    VulkanShader* findShader(ShaderHandle handle) {
        auto it = shaders.find(handle.id);
        return it == shaders.end() ? nullptr : &it->second;
    }
    VulkanPipeline* findPipeline(PipelineHandle handle) {
        auto it = pipelines.find(handle.id);
        return it == pipelines.end() ? nullptr : &it->second;
    }
    VulkanFramebuffer* findTarget(FramebufferHandle handle) {
        auto it = targets.find(handle.id);
        return it == targets.end() ? nullptr : &it->second;
    }

    // Builds (or returns) the VkPipeline of `pipeline` for a target described
    // by its attachment formats.
    VkPipeline pipelineFor(VulkanPipeline& pipeline,
                           const vector<VkFormat>& colorFormats,
                           VkFormat depthFormat, VkFrontFace frontFace);

    uptr<VulkanCommandBuffer> cmd;

    bool createSwapchain();
    void destroySwapchain();
    bool recreateSwapchain();

    // Records `record` into a one-shot command buffer and blocks until the GPU
    // is done. Uploads and readbacks are rare and setup-time, so a simple
    // blocking submit is the right trade; the async transfer queue (a reserved
    // lever, docs/RENDERING.md) can replace it later without touching callers.
    template <typename F>
    bool immediateSubmit(F&& record, bool wait = true);

    VulkanBuffer* findBuffer(BufferHandle handle) {
        auto it = buffers.find(handle.id);
        return it == buffers.end() ? nullptr : &it->second;
    }
    VulkanTexture* findTexture(TextureHandle handle) {
        auto it = textures.find(handle.id);
        return it == textures.end() ? nullptr : &it->second;
    }

    // Single-mip view for storage-image bindings (created on first use).
    VkImageView textureMipView(VulkanTexture& tex, u32 mip) {
        if (mip >= tex.mipLevels) {
            mip = tex.mipLevels - 1;
        }
        if (tex.mipViews.empty()) {
            tex.mipViews.resize(tex.mipLevels, VK_NULL_HANDLE);
        }
        if (tex.mipViews[mip] != VK_NULL_HANDLE) {
            return tex.mipViews[mip];
        }
        VkImageViewCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = tex.image;
        info.viewType = tex.extent.depth > 1  ? VK_IMAGE_VIEW_TYPE_3D
                        : tex.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                              : VK_IMAGE_VIEW_TYPE_2D;
        info.format = tex.format;
        info.subresourceRange.aspectMask = tex.aspect;
        info.subresourceRange.baseMipLevel = mip;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = tex.arrayLayers;
        vkOk(vkCreateImageView(device, &info, nullptr, &tex.mipViews[mip]),
             "vkCreateImageView (storage mip)");
        return tex.mipViews[mip];
    }

    // Allocates a host-visible scratch buffer for one upload/readback.
    bool createStaging(u64 size, bool forRead, VkBuffer& out,
                       VmaAllocation& outAlloc, void** outMapped);
};

template <typename F>
bool VulkanDevice::Impl::immediateSubmit(F&& record, bool wait) {
    VkCommandBufferAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = transferPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!vkOk(vkAllocateCommandBuffers(device, &alloc, &cb),
              "vkAllocateCommandBuffers(transfer)")) {
        return false;
    }

    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    record(cb);
    vkEndCommandBuffer(cb);

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;

    if (!wait) {
        // Async path (buffer uploads): submission ORDER on the single queue
        // already guarantees this copy executes before any later frame's
        // submit, so nothing needs to block (a per-upload fence wait here
        // would stall the streaming budget). The cb (and the caller's
        // staging, parked in pendingBuffers) is freed once the frame slot
        // cycles, which happens-after the copy by the same ordering.
        const bool ok = vkOk(vkQueueSubmit(graphicsQueue, 1, &submit,
                                           VK_NULL_HANDLE),
                             "vkQueueSubmit(transfer)");
        if (ok) {
            pendingCmds.push_back({ cb, asyncParkFrame() });
        } else {
            vkFreeCommandBuffers(device, transferPool, 1, &cb);
        }
        return ok;
    }

    VkFenceCreateInfo fenceInfo {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceInfo, nullptr, &fence);
    const bool ok = vkOk(vkQueueSubmit(graphicsQueue, 1, &submit, fence),
                         "vkQueueSubmit(transfer)");
    if (ok) {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, transferPool, 1, &cb);
    return ok;
}

// Records one frame. Unimplemented commands are no-ops rather than
// asserts — callers must not be able to tell whether a
// backend records or executes immediately (CommandBuffer contract).
class VulkanCommandBuffer final : public CommandBuffer {
public:
    explicit VulkanCommandBuffer(VulkanDevice::Impl& device) : d_ { &device } {}
    // The family this buffer records for: the compute one when it IS the
    // device's async-compute buffer, graphics otherwise.
    VkQueueFlags queueCaps() const {
        return this == d_->computeCmd.get() ? d_->computeQueueFlags
                                            : kAllQueueCaps;
    }

    void begin(VkCommandBuffer cb, VkImageView swapchainView,
               VkFormat swapchainFormat, VkExtent2D extent) {
        cb_ = cb;
        swapchainView_ = swapchainView;
        swapchainFormat_ = swapchainFormat;
        swapchainExtent_ = extent;
        inPass_ = false;
        boundPipeline_ = nullptr;
        boundGroups_ = {};
        pushedMask_ = 0;
    }

    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    void copyBuffer(BufferHandle src, BufferHandle dst, u64 size, u64 srcOffset,
                    u64 dstOffset) override;
    void copyTexture(TextureHandle src, TextureHandle dst) override;

    // Host-visible-buffer update routed through the FRAME command buffer:
    // the barrier orders the copy after every read the in-flight frame may
    // still be doing. updateBuffer's in-place memcpy raced that frame — its
    // passes sampled values meant for the NEXT frame (the CSM matrices
    // flipped mid-frame: flickering dark shadow plates on fast
    // pans). Returns false inside a pass (copies are illegal there)
    // or when nothing is being recorded.
    bool recordHostUpdate(VkBuffer staging, u64 srcOffset, VkBuffer dst,
                          u64 size, u64 dstOffset,
                          VkPipelineStageFlags readStages,
                          VkAccessFlags readAccess);
    bool insidePass() const { return inPass_; }

    void setViewport(u32 x, u32 y, u32 width, u32 height) override;
    void setScissor(u32 x, u32 y, u32 width, u32 height) override;
    void clearScissor() override;
    void setFrontFace(FrontFace frontFace) override;
    void setPipeline(PipelineHandle pipeline) override;
    void setBindGroup(u32 index, BindGroupHandle group) override;
    void pushGroup(BindGroupHandle group);
    void bindMissingDummies();
    void setPushConstants(const void* data, u32 size, u32 offset) override;
    void setVertexBuffer(u32 slot, BufferHandle buffer, u64 offset) override;
    void setIndexBuffer(BufferHandle buffer, IndexFormat format) override;
    void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex) override;
    void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex,
                     u32 firstInstance) override;
    void drawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount,
                             u32 stride) override;
    void dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) override;
    void memoryBarrier(u32 dst) override;
    void readBarrier(u32 src) override;

private:
    // The RHI's viewport origin is bottom-left (the GL convention). Vulkan's
    // is top-left, so every viewport is submitted with a NEGATIVE height,
    // which flips clip space back. Doing it here keeps the flip out of the
    // shared shaders and out of gameplay.
    void applyViewport();

    VulkanDevice::Impl* d_ { nullptr };
    VkCommandBuffer cb_ { VK_NULL_HANDLE };

    // The frame's backbuffer (framebuffer handle 0 targets it).
    VkImageView swapchainView_ { VK_NULL_HANDLE };
    VkFormat swapchainFormat_ { VK_FORMAT_UNDEFINED };
    VkExtent2D swapchainExtent_ {};

    // Target currently being rendered into.
    VkExtent2D extent_ {};
    vector<VkFormat> colorFormats_;
    VkFormat depthFormat_ { VK_FORMAT_UNDEFINED };
    VulkanFramebuffer* target_ { nullptr }; // null = swapchain
    bool inPass_ { false };

    // Dynamic state, re-applied whenever a pipeline is bound.
    VkViewport viewport_ {};
    bool scissorSet_ { false };
    VkRect2D scissor_ {};
    VkFrontFace frontFace_ { VK_FRONT_FACE_COUNTER_CLOCKWISE };
    // GL-meaning winding -> Vulkan winding for the CURRENT target. Offscreen
    // renders through a positive viewport (Vulkan Y-down = one mirror vs GL
    // clip space) so the winding inverts; the swapchain's negative-height
    // viewport cancels that mirror and keeps the GL winding as-is —
    // inverting unconditionally would be wrong on the swapchain.
    VkFrontFace effectiveFrontFace() const {
        if (target_ == nullptr) {
            return frontFace_;
        }
        return frontFace_ == VK_FRONT_FACE_COUNTER_CLOCKWISE
                   ? VK_FRONT_FACE_CLOCKWISE
                   : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    VulkanPipeline* boundPipeline_ { nullptr };
    // Per RHI slot, for the setPipeline replay. Sized for the highest slot
    // the renderer uses (main pass: 0 frame, 2 shadow, 3 clouds, 4 terrain
    // light, 5 key shadow, 6 GI apply) — a slot beyond this array is
    // silently NEVER replayed, which unbinds its descriptors on every
    // pipeline change (the invisible-GI bug).
    array<BindGroupHandle, 12> boundGroups_ {};
    u64 pushedMask_ { 0 }; // bindings pushed since the current pipeline bind
    // Scratch for pushGroup/bindMissingDummies — both run on the per-draw
    // hot path, so the capacity is kept across calls instead of paying a
    // heap allocation per push. The info vectors are RESERVED to the entry
    // count before filling; .back() pointers stored in the writes rely on
    // no reallocation happening mid-fill.
    vector<VkWriteDescriptorSet> scratchWrites_;
    vector<VkDescriptorBufferInfo> scratchBufferInfos_;
    vector<VkDescriptorImageInfo> scratchImageInfos_;
};

} // namespace rhi

#include "engine/rhi/backends/vulkan/VulkanDevice.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <unordered_map>

// Portability-subset structs (MoltenVK feature opt-in) live in vulkan_beta.h.
#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>

#include <shaderc/shaderc.h>

// VMA is header-only; this is the single TU that emits its implementation.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vk_mem_alloc.h>

#include "engine/core/Log.hpp"
#include "engine/platform/VulkanSurface.hpp"
#include "engine/platform/Window.hpp"

namespace rhi {

namespace {

// Frames recorded ahead of the GPU. Per-frame resources (command buffer,
// acquire semaphore, fence) are duplicated this many times so the CPU never
// waits on the frame it is recording. The Phase-5 snapshot seam already passes
// render data by value, so deeper pipelining stays a scheduling choice.
constexpr u32 kFramesInFlight = 2;

// Timestamp slots reserved per frame-in-flight. The GPU-PERF HUD samples a
// handful of scopes per frame; overflowing warns rather than corrupting.
constexpr u32 kTimestampsPerFrame = 64;

bool vkOk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        LOG_ERROR("Vulkan: {} failed (VkResult {})", what,
                  static_cast<i32>(result));
        return false;
    }
    return true;
}

bool hasExtension(const vector<VkExtensionProperties>& available,
                  const char* name) {
    return std::any_of(available.begin(), available.end(),
                       [name](const VkExtensionProperties& e) {
                           return std::strcmp(e.extensionName, name) == 0;
                       });
}

vector<VkExtensionProperties> instanceExtensionProperties() {
    u32 count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    vector<VkExtensionProperties> props(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    return props;
}

vector<VkExtensionProperties> deviceExtensionProperties(VkPhysicalDevice gpu) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, props.data());
    return props;
}

bool validationLayerAvailable() {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(),
                       [](const VkLayerProperties& l) {
                           return std::strcmp(l.layerName,
                                              "VK_LAYER_KHRONOS_validation") ==
                                  0;
                       });
}

// --- Format / state conversion ----------------------------------------------

VkFormat toVkFormat(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:    return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::SRGBA8:   return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureFormat::RGBA16F:  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::R16F:     return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::R32F:     return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::Depth32F: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

bool isDepthFormat(TextureFormat format) {
    return format == TextureFormat::Depth32F;
}

// Bytes per texel, for staging uploads. Only the formats that accept initial
// pixels (RGBA8/SRGBA8) are ever uploaded, but the others are sized here too
// so readbacks/copies can reason about them.
u32 bytesPerTexel(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8:
    case TextureFormat::SRGBA8:   return 4;
    case TextureFormat::RGBA16F:  return 8;
    case TextureFormat::R16F:     return 2;
    case TextureFormat::R32F:
    case TextureFormat::Depth32F: return 4;
    }
    return 4;
}

VkFilter toVkFilter(FilterMode filter) {
    return filter == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode toVkAddressMode(AddressMode mode) {
    return mode == AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                       : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkCompareOp toVkCompareOp(CompareFunc compare) {
    switch (compare) {
    case CompareFunc::Never:        return VK_COMPARE_OP_NEVER;
    case CompareFunc::Less:         return VK_COMPARE_OP_LESS;
    case CompareFunc::Equal:        return VK_COMPARE_OP_EQUAL;
    case CompareFunc::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareFunc::Greater:      return VK_COMPARE_OP_GREATER;
    case CompareFunc::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
    case CompareFunc::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareFunc::Always:       return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_NEVER;
}

VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) {
    switch (usage) {
    case BufferUsage::Vertex:  return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    case BufferUsage::Index:   return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    case BufferUsage::Uniform: return VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    case BufferUsage::Storage: return VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
}

// Access/stage masks for the layout transitions this backend performs. Kept
// deliberately narrow: only the transitions the upload/mipmap/copy paths use.
void layoutMasks(VkImageLayout layout, VkAccessFlags& access,
                 VkPipelineStageFlags& stage) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        access = 0;
        stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        access = VK_ACCESS_TRANSFER_WRITE_BIT;
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        access = VK_ACCESS_TRANSFER_READ_BIT;
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        access = VK_ACCESS_SHADER_READ_BIT;
        stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        break;
    case VK_IMAGE_LAYOUT_GENERAL:
        access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        break;
    default:
        access = 0;
        stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        break;
    }
}

void transitionLayout(VkCommandBuffer cb, VkImage image,
                      VkImageAspectFlags aspect, u32 baseMip, u32 mipCount,
                      u32 layerCount, VkImageLayout oldLayout,
                      VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = baseMip;
    barrier.subresourceRange.levelCount = mipCount;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;

    VkPipelineStageFlags srcStage = 0;
    VkPipelineStageFlags dstStage = 0;
    layoutMasks(oldLayout, barrier.srcAccessMask, srcStage);
    layoutMasks(newLayout, barrier.dstAccessMask, dstStage);

    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

// --- Resource records --------------------------------------------------------

struct VulkanBuffer {
    VkBuffer buffer { VK_NULL_HANDLE };
    VmaAllocation allocation { nullptr };
    u64 size { 0 };
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
    // Tracked so transitions know where they are coming from. One layout for
    // the whole image: this backend never leaves mips in mixed layouts outside
    // of generateMipmaps, which restores a uniform one before returning.
    VkImageLayout layout { VK_IMAGE_LAYOUT_UNDEFINED };
    // Built from TextureDesc::filter/wrap at creation. Used when a bind-group
    // entry carries a texture WITHOUT a sampler — the RHI's legacy 2D
    // contract ("the texture's own creation-time parameters apply"), which GL
    // gets for free from glTexParameter and Vulkan has to spell out.
    VkSampler defaultSampler { VK_NULL_HANDLE };
};

// --- Descriptor binding remap (V4) -------------------------------------------
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

constexpr u32 kBindingRange = 16;

u32 bindingOffset(DescriptorClass klass) {
    switch (klass) {
    case DescriptorClass::Uniform:      return 0 * kBindingRange;
    case DescriptorClass::Sampler:      return 1 * kBindingRange;
    case DescriptorClass::Storage:      return 2 * kBindingRange;
    case DescriptorClass::StorageImage: return 3 * kBindingRange;
    }
    return 0;
}

VkDescriptorType toVkDescriptorType(DescriptorClass klass) {
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

// Rewrites every `layout(... binding = N ...)` so each descriptor class lands
// in its own range, and records what it found. The declarations in the corpus
// are regular enough for this to be exact: a binding qualifier is always
// followed by `uniform <Block|samplerX|imageX>` or `buffer <Block>`.
// SPIR-V does not care which GLSL #version a source declares, but shaderc
// gates features SYNTACTICALLY on it: explicit bindings are 420+, and the
// dual-source 2D shaders declare 410 for the GL 4.1 driver's sake. Promote
// everything to 460 (the corpus standard) before compiling — the GL backends
// never see this rewrite.
str promoteVersion(const str& source) {
    const size_t at = source.find("#version");
    if (at == str::npos) {
        return source;
    }
    const size_t eol = source.find('\n', at);
    if (eol == str::npos) {
        return source;
    }
    str out = source;
    out.replace(at, eol - at, "#version 460 core");
    return out;
}

str remapBindings(const str& source, vector<ShaderResource>& resources) {
    str out;
    out.reserve(source.size() + 64);

    size_t pos = 0;
    while (true) {
        const size_t layoutAt = source.find("layout(", pos);
        if (layoutAt == str::npos) {
            out.append(source, pos, str::npos);
            break;
        }
        const size_t close = source.find(')', layoutAt);
        if (close == str::npos) {
            out.append(source, pos, str::npos);
            break;
        }
        const str layout = source.substr(layoutAt, close - layoutAt + 1);
        const size_t bindingAt = layout.find("binding");
        if (bindingAt == str::npos) {
            out.append(source, pos, close - pos + 1); // location=, std140-only…
            pos = close + 1;
            continue;
        }

        // Parse the binding number out of the qualifier list.
        const size_t digit = layout.find_first_of("0123456789", bindingAt);
        if (digit == str::npos) {
            out.append(source, pos, close - pos + 1);
            pos = close + 1;
            continue;
        }
        size_t digitEnd = layout.find_first_not_of("0123456789", digit);
        if (digitEnd == str::npos) {
            digitEnd = layout.size();
        }
        const u32 original = static_cast<u32>(
            std::stoul(layout.substr(digit, digitEnd - digit)));

        // Classify from the declaration that follows the qualifier, by reading
        // words until the `uniform` or `buffer` keyword: memory qualifiers
        // (readonly/writeonly/coherent) may sit in between.
        DescriptorClass klass = DescriptorClass::Uniform;
        bool comparison = false;
        ImageDim dim = ImageDim::Any;
        size_t at = close + 1;
        auto nextWord = [&]() -> str {
            at = source.find_first_not_of(" \t\r\n", at);
            if (at == str::npos) {
                return {};
            }
            const size_t end = source.find_first_of(" \t\r\n;{(", at);
            const str word = source.substr(
                at, end == str::npos ? str::npos : end - at);
            at = (end == str::npos) ? source.size() : end;
            return word;
        };
        for (u32 guard = 0; guard < 4; ++guard) {
            const str word = nextWord();
            if (word.empty() || word == "buffer") {
                klass = DescriptorClass::Storage;
                break;
            }
            if (word == "uniform") {
                // Memory qualifiers sit on EITHER side of `uniform`
                // (`writeonly uniform image3D` and `uniform writeonly
                // image3D` are both legal GLSL): skip them before reading
                // the type, or a storage image classifies as a uniform
                // block and its binding collides with the real UBOs
                // (found the hard way on rc_inject.comp, V7).
                str type = nextWord();
                for (u32 skip = 0; skip < 4; ++skip) {
                    if (type == "readonly" || type == "writeonly" ||
                        type == "coherent" || type == "volatile" ||
                        type == "restrict") {
                        type = nextWord();
                        continue;
                    }
                    break;
                }
                // find(), not a prefix test: isampler/usampler/uimage/
                // iimage must classify like their float cousins.
                if (type.find("sampler") != str::npos ||
                    type.find("texture") != str::npos) {
                    klass = DescriptorClass::Sampler;
                    comparison = type.find("Shadow") != str::npos;
                    if (type.find("Cube") != str::npos) {
                        dim = ImageDim::Cube;
                    } else if (type.find("3D") != str::npos) {
                        dim = ImageDim::T3D;
                    } else if (type.find("2DArray") != str::npos) {
                        dim = ImageDim::T2DArray;
                    } else if (type.find("2D") != str::npos) {
                        dim = ImageDim::T2D;
                    }
                } else if (type.find("image") != str::npos) {
                    klass = DescriptorClass::StorageImage;
                    if (type.find("3D") != str::npos) {
                        dim = ImageDim::T3D;
                    } else if (type.find("2D") != str::npos) {
                        dim = ImageDim::T2D;
                    }
                } else {
                    klass = DescriptorClass::Uniform; // uniform block
                }
                break;
            }
            // readonly / writeonly / coherent / restrict — keep looking.
        }

        const u32 shifted = original + bindingOffset(klass);
        resources.push_back({ shifted, klass, comparison, dim });

        // Emit the qualifier with the shifted number.
        out.append(source, pos, layoutAt - pos);
        out.append(layout, 0, digit);
        out.append(std::to_string(shifted));
        out.append(layout, digitEnd, str::npos);
        pos = close + 1;
    }
    return out;
}

// Compiles one GLSL stage to SPIR-V. The sources are the SAME GLSL 460 the GL
// backend consumes (ShaderLibrary has already expanded the #includes): the
// corpus carries explicit layout(location=)/layout(binding=) everywhere, and
// the only builtin that differs between the dialects is spelled through
// compat.glsl, keyed on the VULKAN macro that shaderc/glslang predefines.
bool compileToSpv(const str& source, shaderc_shader_kind kind,
                  const str& debugName, vector<u32>& out) {
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_2);
    shaderc_compile_options_set_optimization_level(
        options, shaderc_optimization_level_performance);

    shaderc_compilation_result_t result =
        shaderc_compile_into_spv(compiler, source.c_str(), source.size(), kind,
                                 debugName.c_str(), "main", options);

    const bool ok = shaderc_result_get_compilation_status(result) ==
                    shaderc_compilation_status_success;
    if (ok) {
        const auto* bytes =
            reinterpret_cast<const u32*>(shaderc_result_get_bytes(result));
        const size_t words = shaderc_result_get_length(result) / sizeof(u32);
        out.assign(bytes, bytes + words);
    } else {
        LOG_ERROR("Vulkan shader '{}' failed to compile:\n{}", debugName,
                  shaderc_result_get_error_message(result));
    }
    shaderc_result_release(result);
    shaderc_compile_options_release(options);
    shaderc_compiler_release(compiler);
    return ok;
}

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
u64 targetKey(const vector<VkFormat>& colorFormats, VkFormat depthFormat) {
    u64 key = 1469598103934665603ull; // FNV-1a
    auto mix = [&key](u64 value) {
        key = (key ^ value) * 1099511628211ull;
    };
    for (VkFormat f : colorFormats) {
        mix(static_cast<u64>(f));
    }
    mix(static_cast<u64>(depthFormat) ^ 0x9e3779b9ull);
    return key;
}

class VulkanCommandBuffer;

} // namespace

// --- Device state -------------------------------------------------------------

struct VulkanDevice::Impl {
    platform::Window* window { nullptr };

    VkInstance instance { VK_NULL_HANDLE };
    VkSurfaceKHR surface { VK_NULL_HANDLE };
    VkPhysicalDevice gpu { VK_NULL_HANDLE };
    VkDevice device { VK_NULL_HANDLE };
    VmaAllocator allocator { nullptr };

    u32 graphicsFamily { 0 };
    u32 presentFamily { 0 };
    VkQueue graphicsQueue { VK_NULL_HANDLE };
    VkQueue presentQueue { VK_NULL_HANDLE };

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

    // Mini deletion queue (the V4 'vkDeviceWaitIdle in destroy*' debt, paid
    // where it bit): destroying mid-RECORDING is unsafe even after an idle —
    // the commands referencing the resource are not submitted yet. Parked
    // here and freed once the frame slot cycles (fence-proven done).
    struct PendingTexture {
        VkImage image; VmaAllocation alloc; VkImageView view;
        VkSampler sampler; u64 frame;
    };
    struct PendingBuffer {
        VkBuffer buffer; VmaAllocation alloc; u64 frame;
    };
    vector<PendingTexture> pendingTextures;
    vector<PendingBuffer> pendingBuffers;

    void flushPendingFrees(bool force) {
        const auto done = [&](u64 parked) {
            return force || frameCounter >= parked + kFramesInFlight;
        };
        std::erase_if(pendingTextures, [&](const PendingTexture& t) {
            if (!done(t.frame)) { return false; }
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
    struct PendingTimestamp {
        u32 query { 0 };
        u32 frameSlot { 0 };
        u32 generation { 0 };
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
    // lever, docs/VULKAN.md) can replace it later without touching callers.
    template <typename F>
    bool immediateSubmit(F&& record);

    VulkanBuffer* findBuffer(BufferHandle handle) {
        auto it = buffers.find(handle.id);
        return it == buffers.end() ? nullptr : &it->second;
    }
    VulkanTexture* findTexture(TextureHandle handle) {
        auto it = textures.find(handle.id);
        return it == textures.end() ? nullptr : &it->second;
    }

    // Allocates a host-visible scratch buffer for one upload/readback.
    bool createStaging(u64 size, bool forRead, VkBuffer& out,
                       VmaAllocation& outAlloc, void** outMapped);
};

namespace {

// Records one frame. V1/V2 implement the render pass and the copy commands;
// draws, binds and dispatch land with V4/V5, which is why they are still
// no-ops rather than asserts — callers must not be able to tell whether a
// backend records or executes immediately (CommandBuffer contract).
class VulkanCommandBuffer final : public CommandBuffer {
public:
    explicit VulkanCommandBuffer(VulkanDevice::Impl& device) : d_ { &device } {}

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
    void dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) override;
    void memoryBarrier() override;

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
    // viewport cancels that mirror and keeps the GL winding as-is. The old
    // code inverted unconditionally — wrong on the swapchain, invisible only
    // because those passes cull nothing.
    VkFrontFace effectiveFrontFace() const {
        if (target_ == nullptr) {
            return frontFace_;
        }
        return frontFace_ == VK_FRONT_FACE_COUNTER_CLOCKWISE
                   ? VK_FRONT_FACE_CLOCKWISE
                   : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    VulkanPipeline* boundPipeline_ { nullptr };
    array<BindGroupHandle, 4> boundGroups_ {}; // per RHI slot, for replay
    u64 pushedMask_ { 0 }; // bindings pushed since the current pipeline bind
};

VkAttachmentLoadOp toVkLoadOp(LoadOp op) {
    switch (op) {
    case LoadOp::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

void VulkanCommandBuffer::beginRenderPass(const RenderPassDesc& desc) {
    if (cb_ == VK_NULL_HANDLE || inPass_) {
        return;
    }
    target_ = desc.framebuffer.id == 0 ? nullptr
                                       : d_->findTarget(desc.framebuffer);
    colorFormats_.clear();
    depthFormat_ = VK_FORMAT_UNDEFINED;

    vector<VkRenderingAttachmentInfo> colorAttachments;
    VkRenderingAttachmentInfo depthAttachment {};
    bool hasDepth = false;

    VkClearValue clearColor {};
    clearColor.color = { { desc.clearColor.r, desc.clearColor.g,
                           desc.clearColor.b, desc.clearColor.a } };

    if (target_ == nullptr) {
        // Backbuffer: already moved to COLOR_ATTACHMENT_OPTIMAL by beginFrame.
        extent_ = swapchainExtent_;
        colorFormats_.push_back(swapchainFormat_);
        VkRenderingAttachmentInfo color {};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = swapchainView_;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = toVkLoadOp(desc.loadOp);
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = clearColor;
        colorAttachments.push_back(color);
    } else {
        extent_ = target_->extent;
        colorFormats_ = target_->colorFormats;
        depthFormat_ = target_->depthFormat;
        // Offscreen attachments live in SHADER_READ_ONLY between passes.
        for (size_t i = 0; i < target_->colorViews.size(); ++i) {
            VulkanTexture* tex = d_->findTexture(target_->colorTextures[i]);
            if (tex != nullptr) {
                transitionLayout(cb_, tex->image, tex->aspect, 0,
                                 tex->mipLevels, tex->arrayLayers, tex->layout,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                tex->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            VkRenderingAttachmentInfo color {};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = target_->colorViews[i];
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = toVkLoadOp(desc.loadOp);
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue = clearColor;
            colorAttachments.push_back(color);
        }
        if (target_->depthView != VK_NULL_HANDLE) {
            VulkanTexture* tex = d_->findTexture(target_->depthTexture);
            if (tex != nullptr) {
                transitionLayout(
                    cb_, tex->image, tex->aspect, 0, tex->mipLevels,
                    tex->arrayLayers, tex->layout,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                tex->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }
            depthAttachment.sType =
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = target_->depthView;
            depthAttachment.imageLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = toVkLoadOp(desc.depthLoadOp);
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.clearValue.depthStencil = { desc.clearDepth, 0 };
            hasDepth = true;
        }
    }

    VkRenderingInfo info {};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea.offset = { 0, 0 };
    info.renderArea.extent = extent_;
    info.layerCount = 1;
    info.colorAttachmentCount = static_cast<u32>(colorAttachments.size());
    info.pColorAttachments = colorAttachments.data();
    info.pDepthAttachment = hasDepth ? &depthAttachment : nullptr;
    d_->cmdBeginRendering(cb_, &info);
    inPass_ = true;

    // beginRenderPass resets the dynamic state (CommandBuffer contract).
    boundPipeline_ = nullptr;
    frontFace_ = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    scissorSet_ = false;
    setViewport(0, 0, extent_.width, extent_.height);
    clearScissor();
}

void VulkanCommandBuffer::endRenderPass() {
    if (cb_ == VK_NULL_HANDLE || !inPass_) {
        return;
    }
    d_->cmdEndRendering(cb_);
    inPass_ = false;
    boundPipeline_ = nullptr;

    // Offscreen attachments are sampled by later passes: hand them back in the
    // layout the rest of the backend expects to find them in.
    if (target_ != nullptr) {
        for (TextureHandle handle : target_->colorTextures) {
            VulkanTexture* tex = d_->findTexture(handle);
            if (tex != nullptr) {
                transitionLayout(cb_, tex->image, tex->aspect, 0,
                                 tex->mipLevels, tex->arrayLayers, tex->layout,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        VulkanTexture* depth = d_->findTexture(target_->depthTexture);
        if (depth != nullptr) {
            transitionLayout(cb_, depth->image, depth->aspect, 0,
                             depth->mipLevels, depth->arrayLayers,
                             depth->layout,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            depth->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    target_ = nullptr;
}

void VulkanCommandBuffer::applyViewport() {
    if (cb_ != VK_NULL_HANDLE) {
        vkCmdSetViewport(cb_, 0, 1, &viewport_);
    }
}

void VulkanCommandBuffer::setViewport(u32 x, u32 y, u32 width, u32 height) {
    // The negative-height flip applies ONLY to the swapchain pass. Offscreen
    // passes render Vulkan-natural: GL clip content then lands in GL memory
    // order (row 0 = scene bottom), so every GL-convention shader SAMPLING an
    // offscreen target reads it correctly — flipping everywhere kept each
    // pass upright but inverted the sampling convention, and an odd number
    // of fullscreen hops (tonemap) put the whole image on screen upside
    // down. Only the final swapchain pass needs the flip to turn GL memory
    // order into what the surface presents.
    viewport_.x = static_cast<f32>(x);
    if (target_ == nullptr) {
        viewport_.y = static_cast<f32>(y + height);
        viewport_.height = -static_cast<f32>(height);
    } else {
        viewport_.y = static_cast<f32>(y);
        viewport_.height = static_cast<f32>(height);
    }
    viewport_.width = static_cast<f32>(width);
    viewport_.minDepth = 0.0f;
    viewport_.maxDepth = 1.0f;
    applyViewport();
}

void VulkanCommandBuffer::setScissor(u32 x, u32 y, u32 width, u32 height) {
    // Same per-target rule as the viewport: offscreen rows are already
    // bottom-origin (GL memory order), only the swapchain needs the flip.
    const u32 top =
        target_ != nullptr ? y
        : extent_.height > (y + height) ? extent_.height - (y + height)
                                        : 0u;
    scissor_.offset = { static_cast<i32>(x), static_cast<i32>(top) };
    scissor_.extent = { width, height };
    scissorSet_ = true;
    if (cb_ != VK_NULL_HANDLE) {
        vkCmdSetScissor(cb_, 0, 1, &scissor_);
    }
}

void VulkanCommandBuffer::clearScissor() {
    scissor_.offset = { 0, 0 };
    scissor_.extent = extent_;
    scissorSet_ = false;
    if (cb_ != VK_NULL_HANDLE) {
        vkCmdSetScissor(cb_, 0, 1, &scissor_);
    }
}

void VulkanCommandBuffer::setFrontFace(FrontFace frontFace) {
    // vkCmdSetFrontFace is core only in Vulkan 1.3 (or via
    // extended_dynamic_state), and this backend targets 1.2 — so winding is
    // baked into the pipeline and selected through the variant cache instead,
    // which costs nothing since that cache already exists for target formats.
    const VkFrontFace wanted = frontFace == FrontFace::Clockwise
                                   ? VK_FRONT_FACE_CLOCKWISE
                                   : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (wanted == frontFace_) {
        return;
    }
    frontFace_ = wanted;
    if (boundPipeline_ != nullptr && !boundPipeline_->compute) {
        // Re-resolve the bound pipeline against the new winding.
        VulkanPipeline* pipeline = boundPipeline_;
        boundPipeline_ = nullptr;
        const VkPipeline vk =
            d_->pipelineFor(*pipeline, colorFormats_, depthFormat_,
                            effectiveFrontFace());
        if (vk != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, vk);
            boundPipeline_ = pipeline;
            applyViewport();
            vkCmdSetScissor(cb_, 0, 1, &scissor_);
        }
    }
}

void VulkanCommandBuffer::setPipeline(PipelineHandle handle) {
    VulkanPipeline* pipeline = d_->findPipeline(handle);
    if (cb_ == VK_NULL_HANDLE || pipeline == nullptr) {
        return;
    }
    if (pipeline->compute) {
        if (pipeline->computePipeline != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_COMPUTE,
                              pipeline->computePipeline);
            boundPipeline_ = pipeline;
            pushedMask_ = 0;
            for (const BindGroupHandle g : boundGroups_) {
                if (g.id != 0) {
                    pushGroup(g);
                }
            }
        }
        return;
    }
    // Graphics pipelines are specialized per target format set.
    const VkPipeline vk =
        d_->pipelineFor(*pipeline, colorFormats_, depthFormat_,
                        effectiveFrontFace());
    if (vk == VK_NULL_HANDLE) {
        return;
    }
    vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, vk);
    boundPipeline_ = pipeline;
    // Dynamic state does not survive a pipeline bind.
    applyViewport();
    vkCmdSetScissor(cb_, 0, 1, &scissor_);
    pushedMask_ = 0;
    for (const BindGroupHandle g : boundGroups_) {
        if (g.id != 0) {
            pushGroup(g);
        }
    }
}

void VulkanCommandBuffer::setPushConstants(const void* data, u32 size,
                                           u32 offset) {
    if (cb_ == VK_NULL_HANDLE || boundPipeline_ == nullptr || data == nullptr
        || size == 0) {
        return;
    }
    const u32 declared = boundPipeline_->desc.pushConstantSize;
    if (offset + size > declared) {
        LOG_ERROR("Vulkan setPushConstants: range {}+{} exceeds the pipeline's "
                  "declared pushConstantSize {}",
                  offset, size, declared);
        return;
    }
    // Recorded INTO the command buffer, so the value sticks to the draws that
    // follow it — the whole point (a UBO write would not).
    vkCmdPushConstants(cb_, boundPipeline_->layout,
                       boundPipeline_->compute ? VK_SHADER_STAGE_COMPUTE_BIT
                                               : VK_SHADER_STAGE_ALL_GRAPHICS,
                       offset, size, data);
}

void VulkanCommandBuffer::setBindGroup(u32 index, BindGroupHandle group) {
    // Remembered per slot: push descriptors die with the pipeline layout
    // they were pushed against, but the RHI contract (from GL) is that bind
    // groups SURVIVE a pipeline change — RC binds its frame group once, then
    // switches build/extend/merge pipelines. setPipeline replays these.
    if (index < boundGroups_.size()) {
        boundGroups_[index] = group;
    }
    pushGroup(group);
}

void VulkanCommandBuffer::pushGroup(BindGroupHandle group) {
    auto it = d_->bindGroups.find(group.id);
    if (cb_ == VK_NULL_HANDLE || it == d_->bindGroups.end() ||
        boundPipeline_ == nullptr) {
        return;
    }
    const vector<BindGroupEntry>& entries = it->second.entries;

    // Push descriptors: the writes go straight out against the bound
    // pipeline's layout, so no VkDescriptorSet is ever allocated and there is
    // no layout to match against (docs/VULKAN.md, V4 design).
    vector<VkWriteDescriptorSet> writes;
    vector<VkDescriptorBufferInfo> bufferInfos;
    vector<VkDescriptorImageInfo> imageInfos;
    writes.reserve(entries.size());
    bufferInfos.reserve(entries.size());
    imageInfos.reserve(entries.size());

    for (const BindGroupEntry& entry : entries) {
        VkWriteDescriptorSet write {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.descriptorCount = 1;

        if (entry.buffer.id != 0) {
            VulkanBuffer* buffer = d_->findBuffer(entry.buffer);
            if (buffer == nullptr) {
                continue;
            }
            const DescriptorClass klass = entry.storage
                                              ? DescriptorClass::Storage
                                              : DescriptorClass::Uniform;
            bufferInfos.push_back({ buffer->buffer, 0, VK_WHOLE_SIZE });
            write.dstBinding = entry.binding + bindingOffset(klass);
            write.descriptorType = toVkDescriptorType(klass);
            write.pBufferInfo = &bufferInfos.back();
        } else if (entry.texture.id != 0) {
            VulkanTexture* tex = d_->findTexture(entry.texture);
            const DescriptorClass klass = entry.storageImage
                                              ? DescriptorClass::StorageImage
                                              : DescriptorClass::Sampler;
            if (klass == DescriptorClass::Sampler) {
                // Three situations where THIS texture cannot legally back the
                // binding: it was destroyed; it is still bound as an
                // attachment; or the binding's declared dimensionality
                // differs (binding numbers are reused across shaders — a
                // replayed group may pair a 3D texture with a 2D binding).
                // GL sampled garbage silently in all three; here a dummy of
                // the DECLARED shape stands in so no descriptor stays unset
                // (an unset one is invalid the moment the shader statically
                // reads it).
                const u32 dst = entry.binding + bindingOffset(klass);
                ImageDim declared = ImageDim::Any;
                bool declaredComparison = false;
                for (const ShaderResource& r : boundPipeline_->resources) {
                    if (r.binding == dst) {
                        declared = r.dim;
                        declaredComparison = r.comparison;
                        break;
                    }
                }
                const bool busy =
                    tex != nullptr &&
                    (tex->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                     tex->layout ==
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                const ImageDim actual =
                    tex == nullptr           ? ImageDim::Any
                    : tex->extent.depth > 1  ? ImageDim::T3D
                    : tex->arrayLayers > 1   ? ImageDim::T2DArray
                                             : ImageDim::T2D;
                const bool mismatch = tex != nullptr &&
                                      declared != ImageDim::Any &&
                                      actual != declared;
                if (tex == nullptr || busy || mismatch) {
                    const TextureHandle dummy =
                        declaredComparison               ? d_->dummyDepth
                        : declared == ImageDim::T3D      ? d_->dummy3D
                        : declared == ImageDim::T2DArray ? d_->dummyArray
                                                         : d_->dummy2D;
                    tex = d_->findTexture(dummy);
                    if (tex == nullptr) {
                        continue; // device init itself — dummies not built yet
                    }
                }
            } else if (tex == nullptr) {
                continue;
            }
            VkDescriptorImageInfo image {};
            image.imageView = tex->view;
            if (klass == DescriptorClass::StorageImage) {
                // First storage use moves the image to GENERAL. Legal here:
                // compute runs outside render passes, where barriers are
                // allowed (graphics never binds storage images).
                if (tex->layout != VK_IMAGE_LAYOUT_GENERAL && !inPass_) {
                    transitionLayout(cb_, tex->image, tex->aspect, 0,
                                     tex->mipLevels, tex->arrayLayers,
                                     tex->layout, VK_IMAGE_LAYOUT_GENERAL);
                    tex->layout = VK_IMAGE_LAYOUT_GENERAL;
                }
                image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            } else {
                // The texture's ACTUAL layout, not an assumed one: GPU-written
                // volumes (GI clipmaps, cascades) live in GENERAL for good and
                // are legally sampled from it.
                image.imageLayout = tex->layout;
                auto sampler = d_->samplers.find(entry.sampler.id);
                // No sampler in the entry -> the texture's creation-time
                // parameters apply (legacy 2D contract).
                image.sampler = sampler != d_->samplers.end()
                                    ? sampler->second
                                    : tex->defaultSampler;
                const u32 dst = entry.binding + bindingOffset(klass);
                if (dst < 64 &&
                    ((boundPipeline_->comparisonMask >> dst) & 1ull) != 0) {
                    // The layout's immutable PCF sampler wins; push a plain
                    // sampler so the portability check never sees a mutable
                    // comparison one.
                    image.sampler = tex->defaultSampler;
                }
            }
            imageInfos.push_back(image);
            write.dstBinding = entry.binding + bindingOffset(klass);
            write.descriptorType = toVkDescriptorType(klass);
            write.pImageInfo = &imageInfos.back();
        } else {
            continue;
        }
        // GL semantics: entries the shader does not declare are ignored.
        if (write.dstBinding >= 64 ||
            ((boundPipeline_->bindingMask >> write.dstBinding) & 1ull) == 0) {
            if (!bufferInfos.empty() && write.pBufferInfo != nullptr) {
                bufferInfos.pop_back();
            }
            if (!imageInfos.empty() && write.pImageInfo != nullptr) {
                imageInfos.pop_back();
            }
            continue;
        }
        writes.push_back(write);
    }
    if (writes.empty()) {
        return;
    }
    for (const VkWriteDescriptorSet& w : writes) {
        if (w.dstBinding < 64) {
            pushedMask_ |= 1ull << w.dstBinding;
        }
    }
    // Set 0 ALWAYS: the RHI's namespace is the (class-offset) binding
    // number, not the group slot — the GL backend ignores `index` for the
    // same reason (CommandBuffer.hpp), and every layout here has exactly one
    // set. Slots 0/1/2 (frame / object / caster groups) merge into it.
    d_->cmdPushDescriptorSet(
        cb_,
        boundPipeline_->compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                : VK_PIPELINE_BIND_POINT_GRAPHICS,
        boundPipeline_->layout, 0, static_cast<u32>(writes.size()),
        writes.data());
}

// GL's contract: every statically-read binding is bound to SOMETHING (an
// unbound unit reads garbage but is legal). Vulkan invalidates the whole
// draw/dispatch instead. Any binding the pipeline declares that no group
// pushed gets a dummy of the declared class/shape — GI-off sampling black,
// optional textures, etc. keep working exactly as they did on GL.
void VulkanCommandBuffer::bindMissingDummies() {
    if (cb_ == VK_NULL_HANDLE || boundPipeline_ == nullptr) {
        return;
    }
    const u64 missing = boundPipeline_->bindingMask & ~pushedMask_;
    if (missing == 0) {
        return;
    }
    vector<VkWriteDescriptorSet> writes;
    vector<VkDescriptorBufferInfo> bufferInfos;
    vector<VkDescriptorImageInfo> imageInfos;
    const size_t count = boundPipeline_->resources.size();
    writes.reserve(count);
    bufferInfos.reserve(count);
    imageInfos.reserve(count);
    for (const ShaderResource& r : boundPipeline_->resources) {
        if (r.binding >= 64 || ((missing >> r.binding) & 1ull) == 0) {
            continue;
        }
        VkWriteDescriptorSet write {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = r.binding;
        write.descriptorCount = 1;
        write.descriptorType = toVkDescriptorType(r.klass);
        if (r.klass == DescriptorClass::Uniform ||
            r.klass == DescriptorClass::Storage) {
            VulkanBuffer* buf = d_->findBuffer(
                r.klass == DescriptorClass::Uniform ? d_->dummyUniform
                                                    : d_->dummyStorage);
            if (buf == nullptr) {
                continue;
            }
            VkDescriptorBufferInfo info {};
            info.buffer = buf->buffer;
            info.range = VK_WHOLE_SIZE;
            bufferInfos.push_back(info);
            write.pBufferInfo = &bufferInfos.back();
        } else {
            VulkanTexture* tex = d_->findTexture(
                r.comparison                  ? d_->dummyDepth
                : r.dim == ImageDim::T3D      ? d_->dummy3D
                : r.dim == ImageDim::T2DArray ? d_->dummyArray
                                              : d_->dummy2D);
            if (tex == nullptr) {
                continue;
            }
            VkDescriptorImageInfo image {};
            image.imageView = tex->view;
            if (r.klass == DescriptorClass::StorageImage) {
                if (tex->layout != VK_IMAGE_LAYOUT_GENERAL && !inPass_) {
                    transitionLayout(cb_, tex->image, tex->aspect, 0,
                                     tex->mipLevels, tex->arrayLayers,
                                     tex->layout, VK_IMAGE_LAYOUT_GENERAL);
                    tex->layout = VK_IMAGE_LAYOUT_GENERAL;
                }
                image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            } else {
                image.imageLayout = tex->layout;
                image.sampler = tex->defaultSampler;
            }
            imageInfos.push_back(image);
            write.pImageInfo = &imageInfos.back();
        }
        pushedMask_ |= 1ull << r.binding;
        writes.push_back(write);
    }
    if (writes.empty()) {
        return;
    }
    d_->cmdPushDescriptorSet(
        cb_,
        boundPipeline_->compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                : VK_PIPELINE_BIND_POINT_GRAPHICS,
        boundPipeline_->layout, 0, static_cast<u32>(writes.size()),
        writes.data());
}

void VulkanCommandBuffer::setVertexBuffer(u32 slot, BufferHandle handle,
                                          u64 bufferOffset) {
    VulkanBuffer* buffer = d_->findBuffer(handle);
    if (cb_ == VK_NULL_HANDLE || buffer == nullptr) {
        return;
    }
    const VkDeviceSize offset = bufferOffset;
    vkCmdBindVertexBuffers(cb_, slot, 1, &buffer->buffer, &offset);
}

void VulkanCommandBuffer::setIndexBuffer(BufferHandle handle,
                                         IndexFormat format) {
    VulkanBuffer* buffer = d_->findBuffer(handle);
    if (cb_ == VK_NULL_HANDLE || buffer == nullptr) {
        return;
    }
    vkCmdBindIndexBuffer(cb_, buffer->buffer, 0,
                         format == IndexFormat::U16 ? VK_INDEX_TYPE_UINT16
                                                    : VK_INDEX_TYPE_UINT32);
}

void VulkanCommandBuffer::draw(u32 vertexCount, u32 instanceCount,
                               u32 firstVertex) {
    if (cb_ != VK_NULL_HANDLE) {
        bindMissingDummies();
        vkCmdDraw(cb_, vertexCount, instanceCount, firstVertex, 0);
    }
}

void VulkanCommandBuffer::drawIndexed(u32 indexCount, u32 instanceCount,
                                      u32 firstIndex, u32 firstInstance) {
    if (cb_ != VK_NULL_HANDLE) {
        bindMissingDummies();
        // firstInstance is native here — no GL_ARB_base_instance dance.
        vkCmdDrawIndexed(cb_, indexCount, instanceCount, firstIndex, 0,
                         firstInstance);
    }
}

void VulkanCommandBuffer::dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) {
    if (cb_ != VK_NULL_HANDLE) {
        bindMissingDummies();
        vkCmdDispatch(cb_, groupsX, groupsY, groupsZ);
    }
}

void VulkanCommandBuffer::memoryBarrier() {
    if (cb_ == VK_NULL_HANDLE) {
        return;
    }
    // Deliberately broad: makes compute writes (SSBOs, storage images) visible
    // to everything that follows, which is what the RHI contract promises.
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT |
                            VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
}

void VulkanCommandBuffer::copyBuffer(BufferHandle src, BufferHandle dst,
                                     u64 size, u64 srcOffset, u64 dstOffset) {
    VulkanBuffer* s = d_->findBuffer(src);
    VulkanBuffer* t = d_->findBuffer(dst);
    if (cb_ == VK_NULL_HANDLE || !s || !t) {
        return;
    }
    VkBufferCopy region {};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(cb_, s->buffer, t->buffer, 1, &region);
}

void VulkanCommandBuffer::copyTexture(TextureHandle src, TextureHandle dst) {
    VulkanTexture* s = d_->findTexture(src);
    VulkanTexture* t = d_->findTexture(dst);
    if (cb_ == VK_NULL_HANDLE || !s || !t || inPass_) {
        return; // must be called outside a render pass (CommandBuffer contract)
    }
    const VkImageLayout srcWas = s->layout;
    const VkImageLayout dstWas = t->layout;
    transitionLayout(cb_, s->image, s->aspect, 0, 1, s->arrayLayers, srcWas,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transitionLayout(cb_, t->image, t->aspect, 0, 1, t->arrayLayers, dstWas,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region {};
    region.srcSubresource.aspectMask = s->aspect;
    region.srcSubresource.layerCount = s->arrayLayers;
    region.dstSubresource.aspectMask = t->aspect;
    region.dstSubresource.layerCount = t->arrayLayers;
    region.extent = s->extent;
    vkCmdCopyImage(cb_, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Both are sampled again afterwards (the point of snapshotting).
    transitionLayout(cb_, s->image, s->aspect, 0, 1, s->arrayLayers,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transitionLayout(cb_, t->image, t->aspect, 0, 1, t->arrayLayers,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace

// --- Transfer helpers ----------------------------------------------------------

template <typename F>
bool VulkanDevice::Impl::immediateSubmit(F&& record) {
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

    VkFenceCreateInfo fenceInfo {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceInfo, nullptr, &fence);

    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    const bool ok = vkOk(vkQueueSubmit(graphicsQueue, 1, &submit, fence),
                         "vkQueueSubmit(transfer)");
    if (ok) {
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    }
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, transferPool, 1, &cb);
    return ok;
}

bool VulkanDevice::Impl::createStaging(u64 size, bool forRead, VkBuffer& out,
                                       VmaAllocation& outAlloc,
                                       void** outMapped) {
    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = forRead ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                         : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags =
        VMA_ALLOCATION_CREATE_MAPPED_BIT |
        (forRead ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                 : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

    VmaAllocationInfo allocInfo {};
    if (!vkOk(vmaCreateBuffer(allocator, &info, &alloc, &out, &outAlloc,
                              &allocInfo),
              "vmaCreateBuffer(staging)")) {
        return false;
    }
    *outMapped = allocInfo.pMappedData;
    return true;
}

// --- Swapchain ---------------------------------------------------------------

bool VulkanDevice::Impl::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps {};
    if (!vkOk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }

    // A zero extent means the window is minimized: nothing to create yet.
    extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(static_cast<u32>(window->width()),
                                  caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<u32>(window->height()),
                                   caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    u32 formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr);
    vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount,
                                         formats.data());
    if (formats.empty()) {
        LOG_ERROR("Vulkan: surface reports no formats");
        return false;
    }
    // Prefer a straight 8-bit BGRA UNORM target. The engine's color pipeline
    // manages its own gamma, so an _SRGB swapchain would double-correct.
    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    colorFormat = chosen.format;

    u32 imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const std::array<u32, 2> families { graphicsFamily, presentFamily };
    if (graphicsFamily != presentFamily) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families.data();
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is the only mode guaranteed present everywhere (and is v-synced).
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    info.oldSwapchain = VK_NULL_HANDLE;

    if (!vkOk(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain),
              "vkCreateSwapchainKHR")) {
        return false;
    }

    u32 count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());

    imageViews.resize(count);
    for (u32 i = 0; i < count; ++i) {
        VkImageViewCreateInfo view {};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = colorFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (!vkOk(vkCreateImageView(device, &view, nullptr, &imageViews[i]),
                  "vkCreateImageView")) {
            return false;
        }
    }

    // No VkRenderPass and no VkFramebuffer: with dynamic rendering the
    // attachment, its load/store ops and its clear value are given straight to
    // vkCmdBeginRendering. The layout transitions the render pass used to
    // perform implicitly are issued as explicit barriers in beginFrame/endFrame.
    renderFinished.resize(count);
    for (u32 i = 0; i < count; ++i) {
        VkSemaphoreCreateInfo sem {};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!vkOk(vkCreateSemaphore(device, &sem, nullptr, &renderFinished[i]),
                  "vkCreateSemaphore(renderFinished)")) {
            return false;
        }
    }
    return true;
}

void VulkanDevice::Impl::destroySwapchain() {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    for (VkSemaphore s : renderFinished) {
        vkDestroySemaphore(device, s, nullptr);
    }
    renderFinished.clear();
    for (VkImageView v : imageViews) {
        vkDestroyImageView(device, v, nullptr);
    }
    imageViews.clear();
    images.clear();
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

bool VulkanDevice::Impl::recreateSwapchain() {
    vkDeviceWaitIdle(device);
    destroySwapchain();
    return createSwapchain();
}

// --- Lifetime ----------------------------------------------------------------

VulkanDevice::VulkanDevice() : impl { std::make_unique<Impl>() } {}

VulkanDevice::~VulkanDevice() {
    if (impl->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(impl->device);

        for (auto& [id, fence] : impl->fences) {
            vkDestroyFence(impl->device, fence, nullptr);
        }
        if (impl->queryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(impl->device, impl->queryPool, nullptr);
        }
        for (auto& [id, pipeline] : impl->pipelines) {
            for (auto& [key, variant] : pipeline.variants) {
                vkDestroyPipeline(impl->device, variant, nullptr);
            }
            if (pipeline.computePipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(impl->device, pipeline.computePipeline,
                                  nullptr);
            }
            vkDestroyPipelineLayout(impl->device, pipeline.layout, nullptr);
            vkDestroyDescriptorSetLayout(impl->device, pipeline.setLayout,
                                         nullptr);
        }
        for (auto& [id, target] : impl->targets) {
            for (VkImageView view : target.colorViews) {
                vkDestroyImageView(impl->device, view, nullptr);
            }
            if (target.depthView != VK_NULL_HANDLE) {
                vkDestroyImageView(impl->device, target.depthView, nullptr);
            }
        }
        for (auto& [id, shader] : impl->shaders) {
            vkDestroyShaderModule(impl->device, shader.vertex, nullptr);
            vkDestroyShaderModule(impl->device, shader.fragment, nullptr);
            vkDestroyShaderModule(impl->device, shader.compute, nullptr);
        }
        impl->flushPendingFrees(true);
        for (auto& [id, sampler] : impl->samplers) {
            vkDestroySampler(impl->device, sampler, nullptr);
        }
        if (impl->pcfSampler != VK_NULL_HANDLE) {
            vkDestroySampler(impl->device, impl->pcfSampler, nullptr);
        }
        for (auto& [id, tex] : impl->textures) {
            vkDestroyImageView(impl->device, tex.view, nullptr);
            vmaDestroyImage(impl->allocator, tex.image, tex.allocation);
        }
        for (auto& [id, buf] : impl->buffers) {
            vmaDestroyBuffer(impl->allocator, buf.buffer, buf.allocation);
        }

        for (u32 i = 0; i < kFramesInFlight; ++i) {
            vkDestroySemaphore(impl->device, impl->imageAvailable[i], nullptr);
            vkDestroyFence(impl->device, impl->inFlight[i], nullptr);
        }
        impl->destroySwapchain();
        if (impl->commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl->device, impl->commandPool, nullptr);
        }
        if (impl->transferPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl->device, impl->transferPool, nullptr);
        }
        if (impl->allocator != nullptr) {
            vmaDestroyAllocator(impl->allocator);
        }
        vkDestroyDevice(impl->device, nullptr);
    }
    if (impl->instance != VK_NULL_HANDLE) {
        if (impl->surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(impl->instance, impl->surface, nullptr);
        }
        vkDestroyInstance(impl->instance, nullptr);
    }
}

// --- Frame -------------------------------------------------------------------

CommandBuffer& VulkanDevice::beginFrame() {
    Impl& d = *impl;
    d.frameActive = false;

    vkWaitForFences(d.device, 1, &d.inFlight[d.frame], VK_TRUE, UINT64_MAX);
    d.frameCounter++;
    d.flushPendingFrees(false); // slot fence passed: old parked frees are safe

    VkResult acquired =
        vkAcquireNextImageKHR(d.device, d.swapchain, UINT64_MAX,
                              d.imageAvailable[d.frame], VK_NULL_HANDLE,
                              &d.imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        // Window resized/minimized: rebuild and sit this frame out. The
        // acquire semaphore was not signalled, so nothing must be submitted.
        d.recreateSwapchain();
        return *d.cmd;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        vkOk(acquired, "vkAcquireNextImageKHR");
        return *d.cmd;
    }

    vkResetFences(d.device, 1, &d.inFlight[d.frame]);
    vkResetCommandBuffer(d.commandBuffers[d.frame], 0);

    VkCommandBufferBeginInfo begin {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!vkOk(vkBeginCommandBuffer(d.commandBuffers[d.frame], &begin),
              "vkBeginCommandBuffer")) {
        return *d.cmd;
    }

    // A render pass used to transition the acquired image implicitly; with
    // dynamic rendering it is an explicit barrier. UNDEFINED as the source is
    // correct here — the previous contents are not preserved.
    transitionLayout(d.commandBuffers[d.frame], d.images[d.imageIndex],
                     VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // Recycle this slot's timestamp region: reset is illegal inside a render
    // pass, so it has to happen here, before any beginRenderPass.
    if (d.queryPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(d.commandBuffers[d.frame], d.queryPool,
                            d.frame * kTimestampsPerFrame, kTimestampsPerFrame);
        ++d.regionGeneration[d.frame];
        d.timestampCursor = 0;
    }

    d.cmd->begin(d.commandBuffers[d.frame], d.imageViews[d.imageIndex],
                 d.colorFormat, d.extent);
    d.frameActive = true;
    return *d.cmd;
}

void VulkanDevice::endFrame() {
    Impl& d = *impl;
    if (!d.frameActive) {
        return; // acquire failed this frame — nothing was recorded
    }
    // A caller that forgot endRenderPass would leave the pass open; closing it
    // here keeps the command buffer valid rather than failing the submit.
    d.cmd->endRenderPass();

    VkCommandBuffer cb = d.commandBuffers[d.frame];
    // Presentable layout — again explicit, since no render pass does it.
    transitionLayout(cb, d.images[d.imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, 0,
                     1, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if (!vkOk(vkEndCommandBuffer(cb), "vkEndCommandBuffer")) {
        d.frameActive = false;
        return;
    }

    const VkPipelineStageFlags waitStage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &d.imageAvailable[d.frame];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &d.renderFinished[d.imageIndex];
    if (!vkOk(vkQueueSubmit(d.graphicsQueue, 1, &submit, d.inFlight[d.frame]),
              "vkQueueSubmit")) {
        d.frameActive = false;
        return;
    }

    VkPresentInfoKHR present {};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &d.renderFinished[d.imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &d.swapchain;
    present.pImageIndices = &d.imageIndex;
    const VkResult presented = vkQueuePresentKHR(d.presentQueue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR ||
        presented == VK_SUBOPTIMAL_KHR) {
        d.recreateSwapchain();
    } else {
        vkOk(presented, "vkQueuePresentKHR");
    }

    d.frame = (d.frame + 1) % kFramesInFlight;
    d.frameActive = false;
}

u64 VulkanDevice::nativeTextureId(TextureHandle) const {
    // V6: hand ImGui a VkDescriptorSet for the offscreen target.
    return 0;
}

// --- Buffers -------------------------------------------------------------------

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc,
                                        const void* initialData) {
    Impl& d = *impl;
    if (desc.size == 0) {
        LOG_ERROR("Vulkan createBuffer: zero size");
        return {};
    }
    // Host-visible when the caller rewrites it every frame or reads it back;
    // device-local otherwise, written through a staging copy.
    const bool hostVisible = desc.dynamic || desc.readback;

    VkBufferCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = desc.size;
    info.usage = toVkBufferUsage(desc.usage) |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        alloc.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT |
            (desc.readback
                 ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                 : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
    }

    VulkanBuffer buffer {};
    buffer.size = desc.size;
    VmaAllocationInfo allocInfo {};
    if (!vkOk(vmaCreateBuffer(d.allocator, &info, &alloc, &buffer.buffer,
                              &buffer.allocation, &allocInfo),
              "vmaCreateBuffer")) {
        return {};
    }
    buffer.mapped = hostVisible ? allocInfo.pMappedData : nullptr;

    const u32 id = d.nextId++;
    d.buffers.emplace(id, buffer);
    if (initialData != nullptr) {
        updateBuffer({ id }, initialData, desc.size, 0);
    }
    return { id };
}

void VulkanDevice::updateBuffer(BufferHandle handle, const void* data, u64 size,
                                u64 offset) {
    Impl& d = *impl;
    VulkanBuffer* buffer = d.findBuffer(handle);
    if (!buffer || data == nullptr || size == 0) {
        return;
    }
    if (offset + size > buffer->size) {
        LOG_ERROR("Vulkan updateBuffer: range {}+{} exceeds size {}", offset,
                  size, buffer->size);
        return;
    }

    if (buffer->mapped != nullptr) {
        std::memcpy(static_cast<u8*>(buffer->mapped) + offset, data, size);
        vmaFlushAllocation(d.allocator, buffer->allocation, offset, size);
        return;
    }

    // Device-local: stage then copy on the GPU.
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    void* mapped = nullptr;
    if (!d.createStaging(size, false, staging, stagingAlloc, &mapped)) {
        return;
    }
    std::memcpy(mapped, data, size);
    vmaFlushAllocation(d.allocator, stagingAlloc, 0, size);

    d.immediateSubmit([&](VkCommandBuffer cb) {
        VkBufferCopy region {};
        region.dstOffset = offset;
        region.size = size;
        vkCmdCopyBuffer(cb, staging, buffer->buffer, 1, &region);
    });
    vmaDestroyBuffer(d.allocator, staging, stagingAlloc);
}

// Destroying a resource still referenced by an in-flight command buffer is
// undefined, and OpenGL hid the problem by deferring internally. Until the
// backend keeps a deletion queue (destroy after kFramesInFlight frames), the
// safe move is to drain the GPU first. Destroys are rare — shader hot reload,
// texture eviction, swapchain resize — never per-frame, so the stall is
// acceptable for bring-up. Revisit if a profile ever shows it.
void VulkanDevice::destroyBuffer(BufferHandle handle) {
    Impl& d = *impl;
    auto it = d.buffers.find(handle.id);
    if (it == d.buffers.end()) {
        return;
    }
    if (d.frameActive) { // recorded-but-unsubmitted commands may reference it
        d.pendingBuffers.push_back({ it->second.buffer, it->second.allocation,
                                     d.frameCounter });
        d.buffers.erase(it);
        return;
    }
    vkDeviceWaitIdle(d.device);
    vmaDestroyBuffer(d.allocator, it->second.buffer, it->second.allocation);
    d.buffers.erase(it);
}

void VulkanDevice::readBuffer(BufferHandle handle, void* dst, u64 size,
                              u64 offset) {
    Impl& d = *impl;
    VulkanBuffer* buffer = d.findBuffer(handle);
    if (!buffer || dst == nullptr || size == 0) {
        return;
    }
    if (offset + size > buffer->size) {
        LOG_ERROR("Vulkan readBuffer: range {}+{} exceeds size {}", offset,
                  size, buffer->size);
        return;
    }

    if (buffer->mapped != nullptr) {
        vmaInvalidateAllocation(d.allocator, buffer->allocation, offset, size);
        std::memcpy(dst, static_cast<u8*>(buffer->mapped) + offset, size);
        return;
    }

    // Device-local: copy into a host-visible staging buffer, then read.
    // Callers that do this every frame should set BufferDesc::readback so the
    // buffer is host-visible to begin with (§ Rhi.hpp).
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    void* mapped = nullptr;
    if (!d.createStaging(size, true, staging, stagingAlloc, &mapped)) {
        return;
    }
    d.immediateSubmit([&](VkCommandBuffer cb) {
        VkBufferCopy region {};
        region.srcOffset = offset;
        region.size = size;
        vkCmdCopyBuffer(cb, buffer->buffer, staging, 1, &region);
    });
    vmaInvalidateAllocation(d.allocator, stagingAlloc, 0, size);
    std::memcpy(dst, mapped, size);
    vmaDestroyBuffer(d.allocator, staging, stagingAlloc);
}

// --- Textures ------------------------------------------------------------------

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc,
                                          const void* pixels) {
    Impl& d = *impl;
    if (desc.width == 0 || desc.height == 0) {
        LOG_ERROR("Vulkan createTexture: zero extent");
        return {};
    }
    if (desc.depth > 1 && desc.arrayLayers > 1) {
        LOG_ERROR("Vulkan createTexture: depth and arrayLayers are exclusive");
        return {};
    }

    const bool volume = desc.depth > 1;
    VulkanTexture tex {};
    tex.format = toVkFormat(desc.format);
    tex.extent = { desc.width, desc.height, volume ? desc.depth : 1u };
    tex.mipLevels = std::max(1u, desc.mipLevels);
    tex.arrayLayers = volume ? 1u : std::max(1u, desc.arrayLayers);
    tex.aspect = isDepthFormat(desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                            : VK_IMAGE_ASPECT_COLOR_BIT;

    // SAMPLED unconditionally: in GL every texture is samplable, and the
    // renderer relies on it (shadow depth, scene depth, Hi-Z source are all
    // sampled whatever their declared usage). endRenderPass counts on it too
    // (attachments hand back as SHADER_READ_ONLY).
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.usage & TextureUsage_RenderAttachment) {
        usage |= isDepthFormat(desc.format)
                     ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    // STORAGE wherever the format allows it (GL parity: compute writes 2D
    // targets too — Hi-Z mips, snow mask, cloud bake — not just volumes).
    // SRGB and depth formats do not support storage use.
    if (desc.format != TextureFormat::SRGBA8 &&
        !isDepthFormat(desc.format)) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = tex.format;
    info.extent = tex.extent;
    info.mipLevels = tex.mipLevels;
    info.arrayLayers = tex.arrayLayers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc {};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (!vkOk(vmaCreateImage(d.allocator, &info, &alloc, &tex.image,
                             &tex.allocation, nullptr),
              "vmaCreateImage")) {
        return {};
    }

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = tex.image;
    view.viewType = volume                ? VK_IMAGE_VIEW_TYPE_3D
                    : tex.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                          : VK_IMAGE_VIEW_TYPE_2D;
    view.format = tex.format;
    view.subresourceRange.aspectMask = tex.aspect;
    view.subresourceRange.levelCount = tex.mipLevels;
    view.subresourceRange.layerCount = tex.arrayLayers;
    if (!vkOk(vkCreateImageView(d.device, &view, nullptr, &tex.view),
              "vkCreateImageView(texture)")) {
        vmaDestroyImage(d.allocator, tex.image, tex.allocation);
        return {};
    }

    // Upload the base mip of every layer (tightly packed, layer-major).
    if (pixels != nullptr) {
        const u64 size = static_cast<u64>(desc.width) * desc.height *
                         tex.extent.depth * tex.arrayLayers *
                         bytesPerTexel(desc.format);
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = nullptr;
        void* mapped = nullptr;
        if (d.createStaging(size, false, staging, stagingAlloc, &mapped)) {
            std::memcpy(mapped, pixels, size);
            vmaFlushAllocation(d.allocator, stagingAlloc, 0, size);
            d.immediateSubmit([&](VkCommandBuffer cb) {
                transitionLayout(cb, tex.image, tex.aspect, 0, tex.mipLevels,
                                 tex.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy region {};
                region.imageSubresource.aspectMask = tex.aspect;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = tex.arrayLayers;
                region.imageExtent = tex.extent;
                vkCmdCopyBufferToImage(cb, staging, tex.image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &region);
                transitionLayout(cb, tex.image, tex.aspect, 0, tex.mipLevels,
                                 tex.arrayLayers,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });
            vmaDestroyBuffer(d.allocator, staging, stagingAlloc);
            tex.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    } else {
        // Render targets and GPU-written volumes start empty; move them out of
        // UNDEFINED so the first barrier has a known source layout.
        const VkImageLayout initial =
            volume ? VK_IMAGE_LAYOUT_GENERAL
                   : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        d.immediateSubmit([&](VkCommandBuffer cb) {
            transitionLayout(cb, tex.image, tex.aspect, 0, tex.mipLevels,
                             tex.arrayLayers, VK_IMAGE_LAYOUT_UNDEFINED,
                             initial);
        });
        tex.layout = initial;
    }

    // The creation-time sampler for sampler-less bind entries (legacy 2D).
    VkSamplerCreateInfo samplerInfo {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.minFilter = toVkFilter(desc.filter);
    samplerInfo.magFilter = toVkFilter(desc.filter);
    samplerInfo.mipmapMode = desc.mipLevels > 1
                                 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = toVkAddressMode(desc.wrap);
    samplerInfo.addressModeV = toVkAddressMode(desc.wrap);
    samplerInfo.addressModeW = toVkAddressMode(desc.wrap);
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkOk(vkCreateSampler(d.device, &samplerInfo, nullptr, &tex.defaultSampler),
         "vkCreateSampler (texture default)");

    const u32 id = d.nextId++;
    d.textures.emplace(id, tex);
    return { id };
}

void VulkanDevice::destroyTexture(TextureHandle handle) {
    Impl& d = *impl;
    vkDeviceWaitIdle(d.device);
    auto it = d.textures.find(handle.id);
    if (it == d.textures.end()) {
        return;
    }
    if (d.frameActive) {
        d.pendingTextures.push_back({ it->second.image, it->second.allocation,
                                      it->second.view,
                                      it->second.defaultSampler,
                                      d.frameCounter });
        d.textures.erase(it);
        return;
    }
    vkDestroyImageView(d.device, it->second.view, nullptr);
    if (it->second.defaultSampler != VK_NULL_HANDLE) {
        vkDestroySampler(d.device, it->second.defaultSampler, nullptr);
    }
    vmaDestroyImage(d.allocator, it->second.image, it->second.allocation);
    d.textures.erase(it);
}

void VulkanDevice::generateMipmaps(TextureHandle handle) {
    Impl& d = *impl;
    VulkanTexture* tex = d.findTexture(handle);
    if (!tex || tex->mipLevels <= 1) {
        return;
    }
    // Blitting requires the format to support linear filtering.
    VkFormatProperties props {};
    vkGetPhysicalDeviceFormatProperties(d.gpu, tex->format, &props);
    if (!(props.optimalTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        LOG_ERROR("Vulkan generateMipmaps: format has no linear blit support");
        return;
    }

    const VkImageLayout was = tex->layout;
    d.immediateSubmit([&](VkCommandBuffer cb) {
        // Level 0 becomes the blit source; the rest are transfer destinations.
        transitionLayout(cb, tex->image, tex->aspect, 0, 1, tex->arrayLayers,
                         was, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionLayout(cb, tex->image, tex->aspect, 1, tex->mipLevels - 1,
                         tex->arrayLayers, was,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        i32 width = static_cast<i32>(tex->extent.width);
        i32 height = static_cast<i32>(tex->extent.height);
        i32 depth = static_cast<i32>(tex->extent.depth);
        for (u32 level = 1; level < tex->mipLevels; ++level) {
            const i32 nextW = std::max(1, width / 2);
            const i32 nextH = std::max(1, height / 2);
            const i32 nextD = std::max(1, depth / 2);

            VkImageBlit blit {};
            blit.srcSubresource.aspectMask = tex->aspect;
            blit.srcSubresource.mipLevel = level - 1;
            blit.srcSubresource.layerCount = tex->arrayLayers;
            blit.srcOffsets[1] = { width, height, depth };
            blit.dstSubresource.aspectMask = tex->aspect;
            blit.dstSubresource.mipLevel = level;
            blit.dstSubresource.layerCount = tex->arrayLayers;
            blit.dstOffsets[1] = { nextW, nextH, nextD };
            vkCmdBlitImage(cb, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);

            // This level becomes the next iteration's source.
            transitionLayout(cb, tex->image, tex->aspect, level, 1,
                             tex->arrayLayers,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            width = nextW;
            height = nextH;
            depth = nextD;
        }
        // Every level is now TRANSFER_SRC; leave the whole image sampleable.
        transitionLayout(cb, tex->image, tex->aspect, 0, tex->mipLevels,
                         tex->arrayLayers,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// --- Samplers ------------------------------------------------------------------

SamplerHandle VulkanDevice::createSampler(const SamplerDesc& desc) {
    Impl& d = *impl;
    VkSamplerCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.minFilter = toVkFilter(desc.minFilter);
    info.magFilter = toVkFilter(desc.magFilter);
    info.mipmapMode = desc.mipmapFilter ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = toVkAddressMode(desc.addressU);
    info.addressModeV = toVkAddressMode(desc.addressV);
    info.addressModeW = toVkAddressMode(desc.addressW);
    info.maxLod = VK_LOD_CLAMP_NONE;
    if (desc.maxAnisotropy > 1.0f) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = desc.maxAnisotropy;
    }
    // CompareFunc::Never means "not a comparison sampler" (§ Rhi.hpp), which
    // is why it maps to disabled rather than to VK_COMPARE_OP_NEVER.
    if (desc.compare != CompareFunc::Never) {
        info.compareEnable = VK_TRUE;
        info.compareOp = toVkCompareOp(desc.compare);
    }

    VkSampler sampler = VK_NULL_HANDLE;
    if (!vkOk(vkCreateSampler(d.device, &info, nullptr, &sampler),
              "vkCreateSampler")) {
        return {};
    }
    const u32 id = d.nextId++;
    d.samplers.emplace(id, sampler);
    return { id };
}

void VulkanDevice::destroySampler(SamplerHandle handle) {
    Impl& d = *impl;
    vkDeviceWaitIdle(d.device);
    auto it = d.samplers.find(handle.id);
    if (it == d.samplers.end()) {
        return;
    }
    vkDestroySampler(d.device, it->second, nullptr);
    d.samplers.erase(it);
}

// --- Pipelines / bind groups / queries (V3, V4, V6) ---------------------------

// --- Render targets -------------------------------------------------------

FramebufferHandle VulkanDevice::createFramebuffer(const FramebufferDesc& desc) {
    Impl& d = *impl;
    VulkanFramebuffer target {};

    // A FramebufferAttachment selects one mip of one layer, so each needs its
    // own view rather than the texture's whole-resource view.
    auto makeView = [&](const FramebufferAttachment& attachment,
                        VkImageView& out, VkFormat& format,
                        u32& width, u32& height) {
        VulkanTexture* tex = d.findTexture(attachment.texture);
        if (tex == nullptr) {
            return false;
        }
        VkImageViewCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = tex->image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = tex->format;
        info.subresourceRange.aspectMask = tex->aspect;
        info.subresourceRange.baseMipLevel = attachment.mipLevel;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = attachment.arrayLayer;
        info.subresourceRange.layerCount = 1;
        if (!vkOk(vkCreateImageView(d.device, &info, nullptr, &out),
                  "vkCreateImageView(attachment)")) {
            return false;
        }
        format = tex->format;
        width = std::max(1u, tex->extent.width >> attachment.mipLevel);
        height = std::max(1u, tex->extent.height >> attachment.mipLevel);
        return true;
    };

    u32 width = 0;
    u32 height = 0;
    for (const FramebufferAttachment& attachment : desc.colorAttachments) {
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        if (!makeView(attachment, view, format, width, height)) {
            LOG_ERROR("Vulkan createFramebuffer: bad color attachment");
            return {};
        }
        target.colorViews.push_back(view);
        target.colorFormats.push_back(format);
        target.colorTextures.push_back(attachment.texture);
    }
    if (desc.depthAttachment.texture.id != 0) {
        if (!makeView(desc.depthAttachment, target.depthView,
                      target.depthFormat, width, height)) {
            LOG_ERROR("Vulkan createFramebuffer: bad depth attachment");
            return {};
        }
        target.depthTexture = desc.depthAttachment.texture;
    }
    if (width == 0 || height == 0) {
        LOG_ERROR("Vulkan createFramebuffer: no attachments");
        return {};
    }
    target.extent = { width, height };

    const u32 id = d.nextId++;
    d.targets.emplace(id, std::move(target));
    return { id };
}

void VulkanDevice::destroyFramebuffer(FramebufferHandle handle) {
    Impl& d = *impl;
    vkDeviceWaitIdle(d.device);
    auto it = d.targets.find(handle.id);
    if (it == d.targets.end()) {
        return;
    }
    for (VkImageView view : it->second.colorViews) {
        vkDestroyImageView(d.device, view, nullptr);
    }
    if (it->second.depthView != VK_NULL_HANDLE) {
        vkDestroyImageView(d.device, it->second.depthView, nullptr);
    }
    d.targets.erase(it);
}

// ShaderDesc::uniformBlocks / ::samplers are deliberately ignored here: they
// exist so the GL backend can assign bindings AFTER link (GLSL 4.10 has no
// layout(binding=)). The shader corpus already carries explicit binding
// qualifiers, which SPIR-V reads directly — so on Vulkan there is nothing to
// patch up.
ShaderHandle VulkanDevice::createShader(const ShaderDesc& desc) {
    Impl& d = *impl;
    const bool isCompute = !desc.computeSource.empty();
    if (isCompute &&
        (!desc.vertexSource.empty() || !desc.fragmentSource.empty())) {
        LOG_ERROR("Vulkan createShader '{}': compute excludes vertex/fragment",
                  desc.debugName);
        return {};
    }
    if (!isCompute &&
        (desc.vertexSource.empty() || desc.fragmentSource.empty())) {
        LOG_ERROR("Vulkan createShader '{}': needs vertex + fragment",
                  desc.debugName);
        return {};
    }

    VulkanShader shader {};

    auto build = [&](const str& source, shaderc_shader_kind kind,
                     const char* stage, VkShaderModule& module) {
        // Shift each descriptor class into its own binding range (GL's
        // separate namespaces do not survive into Vulkan) and record what the
        // stage declares, for the pipeline layout.
        const str translated =
            remapBindings(promoteVersion(source), shader.resources);
        vector<u32> spv;
        if (!compileToSpv(translated, kind, desc.debugName + "." + stage,
                          spv)) {
            return false;
        }
        VkShaderModuleCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = spv.size() * sizeof(u32);
        info.pCode = spv.data();
        return vkOk(vkCreateShaderModule(d.device, &info, nullptr, &module),
                    "vkCreateShaderModule");
    };

    bool ok = true;
    if (isCompute) {
        ok = build(desc.computeSource, shaderc_compute_shader, "comp",
                   shader.compute);
    } else {
        ok = build(desc.vertexSource, shaderc_vertex_shader, "vert",
                   shader.vertex) &&
             build(desc.fragmentSource, shaderc_fragment_shader, "frag",
                   shader.fragment);
    }
    if (!ok) {
        // Partial success still leaves a module behind; drop it.
        vkDestroyShaderModule(d.device, shader.vertex, nullptr);
        vkDestroyShaderModule(d.device, shader.fragment, nullptr);
        vkDestroyShaderModule(d.device, shader.compute, nullptr);
        return {};
    }

    // Vertex and fragment legitimately declare the same resource (FrameUbo…):
    // collapse duplicates, then assert the remap actually separated the
    // classes. A leftover clash would only surface as an invalid descriptor
    // set layout much later, so it is caught loudly here instead.
    std::sort(shader.resources.begin(), shader.resources.end(),
              [](const ShaderResource& a, const ShaderResource& b) {
                  return a.binding < b.binding;
              });
    shader.resources.erase(
        std::unique(shader.resources.begin(), shader.resources.end(),
                    [](const ShaderResource& a, const ShaderResource& b) {
                        return a.binding == b.binding && a.klass == b.klass;
                    }),
        shader.resources.end());
    for (size_t i = 1; i < shader.resources.size(); ++i) {
        if (shader.resources[i].binding == shader.resources[i - 1].binding) {
            LOG_ERROR("Vulkan createShader '{}': binding {} claimed by two "
                      "descriptor classes after remap",
                      desc.debugName, shader.resources[i].binding);
            vkDestroyShaderModule(d.device, shader.vertex, nullptr);
            vkDestroyShaderModule(d.device, shader.fragment, nullptr);
            vkDestroyShaderModule(d.device, shader.compute, nullptr);
            return {};
        }
    }

    const u32 id = d.nextId++;
    d.shaders.emplace(id, shader);
    return { id };
}

void VulkanDevice::destroyShader(ShaderHandle handle) {
    Impl& d = *impl;
    vkDeviceWaitIdle(d.device);
    auto it = d.shaders.find(handle.id);
    if (it == d.shaders.end()) {
        return;
    }
    vkDestroyShaderModule(d.device, it->second.vertex, nullptr);
    vkDestroyShaderModule(d.device, it->second.fragment, nullptr);
    vkDestroyShaderModule(d.device, it->second.compute, nullptr);
    d.shaders.erase(it);
}

// --- Pipelines --------------------------------------------------------------

namespace {

VkPrimitiveTopology toVkTopology(PrimitiveTopology topology) {
    switch (topology) {
    case PrimitiveTopology::Triangles:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::TriangleStrip:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PrimitiveTopology::Lines: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkFormat toVkVertexFormat(VertexFormat format) {
    switch (format) {
    case VertexFormat::F32x1: return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::F32x2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::F32x3: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::F32x4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_R32G32B32A32_SFLOAT;
}

VkCullModeFlags toVkCullMode(CullMode cull) {
    switch (cull) {
    case CullMode::None:  return VK_CULL_MODE_NONE;
    case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
    case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_NONE;
}

void applyBlend(BlendMode mode, VkPipelineColorBlendAttachmentState& state) {
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    state.blendEnable = mode == BlendMode::Opaque ? VK_FALSE : VK_TRUE;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    switch (mode) {
    case BlendMode::Opaque:
        break;
    case BlendMode::Alpha:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::PremultipliedAlpha:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
}

} // namespace

VkPipeline VulkanDevice::Impl::pipelineFor(VulkanPipeline& pipeline,
                                           const vector<VkFormat>& colorFormats,
                                           VkFormat depthFormat,
                                           VkFrontFace frontFace) {
    const u64 key = targetKey(colorFormats, depthFormat) ^
                    (frontFace == VK_FRONT_FACE_CLOCKWISE ? 0x5bf03635ull : 0);
    auto cached = pipeline.variants.find(key);
    if (cached != pipeline.variants.end()) {
        return cached->second;
    }

    VulkanShader* shader = findShader(pipeline.desc.shader);
    if (shader == nullptr || shader->vertex == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan pipeline: shader has no graphics stages");
        return VK_NULL_HANDLE;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shader->vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shader->fragment;
    stages[1].pName = "main";

    vector<VkVertexInputBindingDescription> bindings;
    vector<VkVertexInputAttributeDescription> attributes;
    for (size_t slot = 0; slot < pipeline.desc.vertexBuffers.size(); ++slot) {
        const VertexBufferLayout& layout = pipeline.desc.vertexBuffers[slot];
        VkVertexInputBindingDescription binding {};
        binding.binding = static_cast<u32>(slot);
        binding.stride = layout.stride;
        binding.inputRate = layout.stepMode == VertexStepMode::Instance
                                ? VK_VERTEX_INPUT_RATE_INSTANCE
                                : VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(binding);
        for (const VertexAttribute& attr : layout.attributes) {
            VkVertexInputAttributeDescription a {};
            a.location = attr.location;
            a.binding = static_cast<u32>(slot);
            a.format = toVkVertexFormat(attr.format);
            a.offset = attr.offset;
            attributes.push_back(a);
        }
    }

    VkPipelineVertexInputStateCreateInfo vertexInput {};
    vertexInput.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<u32>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<u32>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly {};
    assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = toVkTopology(pipeline.desc.topology);

    VkPipelineViewportStateCreateInfo viewportState {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster {};
    raster.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = pipeline.desc.wireframe ? VK_POLYGON_MODE_LINE
                                                 : VK_POLYGON_MODE_FILL;
    raster.cullMode = toVkCullMode(pipeline.desc.cull);
    // Already the EFFECTIVE winding (per-target mirror folded in by
    // effectiveFrontFace() at the call sites).
    raster.frontFace = frontFace;
    raster.lineWidth = 1.0f;
    if (pipeline.desc.depthBias != 0.0f ||
        pipeline.desc.depthBiasSlope != 0.0f) {
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = pipeline.desc.depthBias;
        raster.depthBiasSlopeFactor = pipeline.desc.depthBiasSlope;
    }

    VkPipelineMultisampleStateCreateInfo multisample {};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil {};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable =
        pipeline.desc.depth.testEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable =
        pipeline.desc.depth.writeEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = toVkCompareOp(pipeline.desc.depth.compare);

    vector<VkPipelineColorBlendAttachmentState> blendStates(
        std::max<size_t>(1, colorFormats.size()));
    for (VkPipelineColorBlendAttachmentState& state : blendStates) {
        applyBlend(pipeline.desc.blend, state);
    }
    VkPipelineColorBlendStateCreateInfo blend {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<u32>(colorFormats.size());
    blend.pAttachments = blendStates.data();

    const std::array<VkDynamicState, 2> dynamicStates {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<u32>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    // Dynamic rendering replaces the render pass: the pipeline is told the
    // attachment formats directly.
    VkPipelineRenderingCreateInfo rendering {};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = static_cast<u32>(colorFormats.size());
    rendering.pColorAttachmentFormats = colorFormats.data();
    rendering.depthAttachmentFormat = depthFormat;

    VkGraphicsPipelineCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline.layout;

    VkPipeline vk = VK_NULL_HANDLE;
    if (!vkOk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info,
                                        nullptr, &vk),
              "vkCreateGraphicsPipelines")) {
        return VK_NULL_HANDLE;
    }
    pipeline.variants.emplace(key, vk);
    return vk;
}

// Builds the descriptor set layout + pipeline layout from the shader's
// reflection. PUSH_DESCRIPTOR_BIT is what lets setBindGroup write through it
// without ever allocating a VkDescriptorSet.
bool buildLayouts(VkDevice device, VkSampler pcfSampler,
                  const VulkanShader& shader, VulkanPipeline& pipeline) {
    pipeline.resources = shader.resources;
    vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(shader.resources.size());
    for (const ShaderResource& resource : shader.resources) {
        VkDescriptorSetLayoutBinding binding {};
        binding.binding = resource.binding;
        binding.descriptorType = toVkDescriptorType(resource.klass);
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_ALL;
        if (resource.comparison && pcfSampler != VK_NULL_HANDLE) {
            // Immutable: MoltenVK cannot take comparison samplers as pushed
            // descriptors (mutableComparisonSamplers unsupported on M1).
            binding.pImmutableSamplers = &pcfSampler;
            if (resource.binding < 64) {
                pipeline.comparisonMask |= 1ull << resource.binding;
            }
        }
        bindings.push_back(binding);
        if (resource.binding < 64) {
            pipeline.bindingMask |= 1ull << resource.binding;
        }
    }

    VkDescriptorSetLayoutCreateInfo setInfo {};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = static_cast<u32>(bindings.size());
    setInfo.pBindings = bindings.data();
    if (!vkOk(vkCreateDescriptorSetLayout(device, &setInfo, nullptr,
                                          &pipeline.setLayout),
              "vkCreateDescriptorSetLayout")) {
        return false;
    }

    VkPipelineLayoutCreateInfo layoutInfo {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &pipeline.setLayout;
    // The range must be declared here: unlike a descriptor, push-constant
    // storage is part of the layout itself.
    VkPushConstantRange pushRange {};
    if (pipeline.desc.pushConstantSize > 0) {
        // The RHI does not model per-stage visibility, so every stage of the
        // pipeline's own kind sees the range.
        pushRange.stageFlags = pipeline.compute ? VK_SHADER_STAGE_COMPUTE_BIT
                                                : VK_SHADER_STAGE_ALL_GRAPHICS;
        pushRange.offset = 0;
        pushRange.size = pipeline.desc.pushConstantSize;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
    }
    return vkOk(vkCreatePipelineLayout(device, &layoutInfo, nullptr,
                                       &pipeline.layout),
                "vkCreatePipelineLayout");
}

PipelineHandle VulkanDevice::createPipeline(const PipelineDesc& desc) {
    Impl& d = *impl;
    VulkanShader* shader = d.findShader(desc.shader);
    if (shader == nullptr || shader->vertex == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan createPipeline: invalid or non-graphics shader");
        return {};
    }
    VulkanPipeline pipeline {};
    pipeline.desc = desc;
    if (!buildLayouts(d.device, d.pcfSampler, *shader, pipeline)) {
        return {};
    }
    // The VkPipeline itself waits for the first setPipeline, when the target's
    // attachment formats are known (PipelineDesc does not carry them).
    const u32 id = d.nextId++;
    d.pipelines.emplace(id, std::move(pipeline));
    return { id };
}

PipelineHandle VulkanDevice::createComputePipeline(
    const ComputePipelineDesc& desc) {
    Impl& d = *impl;
    VulkanShader* shader = d.findShader(desc.shader);
    if (shader == nullptr || shader->compute == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan createComputePipeline: shader has no compute stage");
        return {};
    }
    VulkanPipeline pipeline {};
    pipeline.compute = true;
    pipeline.desc.shader = desc.shader;
    pipeline.desc.pushConstantSize = desc.pushConstantSize;
    if (!buildLayouts(d.device, d.pcfSampler, *shader, pipeline)) {
        return {};
    }

    VkPipelineShaderStageCreateInfo stage {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader->compute;
    stage.pName = "main";

    VkComputePipelineCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage = stage;
    info.layout = pipeline.layout;
    if (!vkOk(vkCreateComputePipelines(d.device, VK_NULL_HANDLE, 1, &info,
                                       nullptr, &pipeline.computePipeline),
              "vkCreateComputePipelines")) {
        vkDestroyPipelineLayout(d.device, pipeline.layout, nullptr);
        vkDestroyDescriptorSetLayout(d.device, pipeline.setLayout, nullptr);
        return {};
    }

    const u32 id = d.nextId++;
    d.pipelines.emplace(id, std::move(pipeline));
    return { id };
}

void VulkanDevice::destroyPipeline(PipelineHandle handle) {
    Impl& d = *impl;
    vkDeviceWaitIdle(d.device);
    auto it = d.pipelines.find(handle.id);
    if (it == d.pipelines.end()) {
        return;
    }
    for (auto& [key, variant] : it->second.variants) {
        vkDestroyPipeline(d.device, variant, nullptr);
    }
    if (it->second.computePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(d.device, it->second.computePipeline, nullptr);
    }
    vkDestroyPipelineLayout(d.device, it->second.layout, nullptr);
    vkDestroyDescriptorSetLayout(d.device, it->second.setLayout, nullptr);
    d.pipelines.erase(it);
}

// --- GPU markers ------------------------------------------------------------

FenceHandle VulkanDevice::insertFence() {
    Impl& d = *impl;
    VkFenceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (!vkOk(vkCreateFence(d.device, &info, nullptr, &fence),
              "vkCreateFence(marker)")) {
        return {};
    }
    // An empty submit signals only once every previously submitted command has
    // completed — the GL sync-object semantics the RHI describes.
    if (!vkOk(vkQueueSubmit(d.graphicsQueue, 0, nullptr, fence),
              "vkQueueSubmit(fence marker)")) {
        vkDestroyFence(d.device, fence, nullptr);
        return {};
    }
    const u32 id = d.nextId++;
    d.fences.emplace(id, fence);
    return { id };
}

bool VulkanDevice::fenceReady(FenceHandle handle) {
    Impl& d = *impl;
    auto it = d.fences.find(handle.id);
    if (it == d.fences.end()) {
        return true; // unknown or already consumed
    }
    // Polling only: never block the frame thread (Phase-5 completion queue).
    if (vkGetFenceStatus(d.device, it->second) != VK_SUCCESS) {
        return false;
    }
    vkDestroyFence(d.device, it->second, nullptr); // handles are single-use
    d.fences.erase(it);
    return true;
}

void VulkanDevice::destroyFence(FenceHandle handle) {
    Impl& d = *impl;
    auto it = d.fences.find(handle.id);
    if (it == d.fences.end()) {
        return;
    }
    vkDestroyFence(d.device, it->second, nullptr);
    d.fences.erase(it);
}

TimestampHandle VulkanDevice::insertTimestamp() {
    Impl& d = *impl;
    if (d.queryPool == VK_NULL_HANDLE || !d.frameActive) {
        return {}; // only meaningful while a frame is recording
    }
    if (d.timestampCursor >= kTimestampsPerFrame) {
        LOG_WARN("Vulkan: more than {} timestamps in one frame — dropping",
                 kTimestampsPerFrame);
        return {};
    }
    const u32 query = d.frame * kTimestampsPerFrame + d.timestampCursor++;
    // BOTTOM_OF_PIPE: the clock is taken when the stream REACHES this point.
    vkCmdWriteTimestamp(d.commandBuffers[d.frame],
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.queryPool,
                        query);
    const u32 id = d.nextId++;
    d.timestamps.emplace(id, Impl::PendingTimestamp {
                                 query, d.frame, d.regionGeneration[d.frame] });
    return { id };
}

bool VulkanDevice::timestampReady(TimestampHandle handle, u64& nanos) {
    Impl& d = *impl;
    auto it = d.timestamps.find(handle.id);
    if (it == d.timestamps.end()) {
        return false;
    }
    const Impl::PendingTimestamp pending = it->second;
    // Its region has been recycled since: the result is gone for good, so
    // release the handle rather than reporting another frame's number.
    if (pending.generation != d.regionGeneration[pending.frameSlot]) {
        d.timestamps.erase(it);
        return false;
    }
    u64 ticks = 0;
    const VkResult result = vkGetQueryPoolResults(
        d.device, d.queryPool, pending.query, 1, sizeof(ticks), &ticks,
        sizeof(ticks), VK_QUERY_RESULT_64_BIT);
    if (result != VK_SUCCESS) {
        return false; // VK_NOT_READY — poll again next frame, never block
    }
    nanos = static_cast<u64>(static_cast<f64>(ticks) * d.timestampPeriod);
    d.timestamps.erase(it); // single-use, like fences
    return true;
}

void VulkanDevice::destroyTimestamp(TimestampHandle handle) {
    impl->timestamps.erase(handle.id);
}

// Push descriptors mean there is nothing to allocate here: a bind group is the
// list of writes, replayed against whatever pipeline layout is bound.
BindGroupHandle VulkanDevice::createBindGroup(const BindGroupDesc& desc) {
    Impl& d = *impl;
    const u32 id = d.nextId++;
    d.bindGroups.emplace(id, VulkanBindGroup { desc.entries });
    return { id };
}

void VulkanDevice::destroyBindGroup(BindGroupHandle handle) {
    impl->bindGroups.erase(handle.id);
}

// --- Creation ----------------------------------------------------------------

uptr<VulkanDevice> VulkanDevice::create(platform::Window& window) {
    auto self = uptr<VulkanDevice> { new VulkanDevice() };
    Impl& d = *self->impl;
    d.window = &window;

    // --- Instance ---
    vector<const char*> extensions = platform::vulkanInstanceExtensions();
    if (extensions.empty()) {
        return nullptr;
    }
    const vector<VkExtensionProperties> availableExt =
        instanceExtensionProperties();

    VkInstanceCreateFlags instanceFlags = 0;
    // MoltenVK is a *portability* driver: it is only enumerated when the
    // instance opts in explicitly.
    if (hasExtension(availableExt,
                     VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    vector<const char*> layers;
#ifndef NDEBUG
    if (validationLayerAvailable()) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        LOG_DEBUG("Vulkan: validation layer enabled");
    }
#endif

    VkApplicationInfo app {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Meadows";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceInfo {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.flags = instanceFlags;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledLayerCount = static_cast<u32>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    if (!vkOk(vkCreateInstance(&instanceInfo, nullptr, &d.instance),
              "vkCreateInstance")) {
        return nullptr;
    }

    // --- Surface ---
    d.surface = reinterpret_cast<VkSurfaceKHR>(
        platform::createVulkanSurface(window, d.instance));
    if (d.surface == VK_NULL_HANDLE) {
        return nullptr;
    }

    // --- Physical device ---
    u32 gpuCount = 0;
    vkEnumeratePhysicalDevices(d.instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG_ERROR("Vulkan: no physical device found");
        return nullptr;
    }
    vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(d.instance, &gpuCount, gpus.data());

    i32 bestScore = -1;
    for (VkPhysicalDevice candidate : gpus) {
        const vector<VkExtensionProperties> devExtProps =
            deviceExtensionProperties(candidate);
        if (!hasExtension(devExtProps, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }
        u32 familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                                 nullptr);
        vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount,
                                                 families.data());
        bool haveGraphics = false;
        bool havePresent = false;
        u32 graphics = 0;
        u32 present = 0;
        for (u32 i = 0; i < familyCount; ++i) {
            if (!haveGraphics &&
                (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                graphics = i;
                haveGraphics = true;
            }
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, d.surface,
                                                 &supported);
            if (!havePresent && supported) {
                present = i;
                havePresent = true;
            }
        }
        if (!haveGraphics || !havePresent) {
            continue;
        }
        VkPhysicalDeviceProperties candidateProps {};
        vkGetPhysicalDeviceProperties(candidate, &candidateProps);
        // Prefer a discrete GPU; on the M1 there is only the integrated one.
        const i32 score =
            candidateProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                ? 2
                : 1;
        if (score > bestScore) {
            bestScore = score;
            d.gpu = candidate;
            d.graphicsFamily = graphics;
            d.presentFamily = present;
        }
    }
    if (d.gpu == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan: no device with graphics + present + swapchain");
        return nullptr;
    }

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(d.gpu, &props);
    VkPhysicalDeviceFeatures features {};
    vkGetPhysicalDeviceFeatures(d.gpu, &features);

    // --- Logical device ---
    vector<const char*> deviceExtensions { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const vector<VkExtensionProperties> devExt =
        deviceExtensionProperties(d.gpu);
    // Mandatory when present (MoltenVK): the spec requires enabling it on a
    // portability driver.
    VkPhysicalDevicePortabilitySubsetFeaturesKHR portability {};
    portability.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
    const bool onPortability =
        hasExtension(devExt, "VK_KHR_portability_subset");
    if (onPortability) {
        deviceExtensions.push_back("VK_KHR_portability_subset");
        // Features default to FALSE unless explicitly enabled at device
        // creation: query what the driver supports and enable ALL of it.
        // The one that bites first is mutableComparisonSamplers — without it
        // MoltenVK rejects pushing PCF (comparison) samplers as descriptors.
        VkPhysicalDeviceFeatures2 query {};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        query.pNext = &portability;
        vkGetPhysicalDeviceFeatures2(d.gpu, &query);
    }
    // Both verified present on MoltenVK/M1 (docs/VULKAN.md). Dynamic rendering
    // removes VkRenderPass/VkFramebuffer entirely; push descriptors let
    // setBindGroup write through the bound pipeline's layout, which is what
    // makes the RHI's shader-agnostic BindGroupDesc expressible at all.
    if (!hasExtension(devExt, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ||
        !hasExtension(devExt, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        LOG_ERROR("Vulkan: device lacks dynamic_rendering and/or "
                  "push_descriptor — both are required by this backend");
        return nullptr;
    }
    deviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    deviceExtensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

    // Only anisotropy is requested, and only if the GPU has it — samplers fall
    // back to isotropic filtering otherwise.
    VkPhysicalDeviceFeatures enabled {};
    enabled.samplerAnisotropy = features.samplerAnisotropy;

    const f32 priority = 1.0f;
    vector<VkDeviceQueueCreateInfo> queueInfos;
    VkDeviceQueueCreateInfo graphicsQueueInfo {};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = d.graphicsFamily;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &priority;
    queueInfos.push_back(graphicsQueueInfo);
    if (d.presentFamily != d.graphicsFamily) {
        VkDeviceQueueCreateInfo presentQueueInfo = graphicsQueueInfo;
        presentQueueInfo.queueFamilyIndex = d.presentFamily;
        queueInfos.push_back(presentQueueInfo);
    }

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering {};
    dynamicRendering.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    dynamicRendering.dynamicRendering = VK_TRUE;

    VkDeviceCreateInfo deviceInfo {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &dynamicRendering;
    if (onPortability) {
        portability.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &portability;
    }
    deviceInfo.queueCreateInfoCount = static_cast<u32>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<u32>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &enabled;
    if (!vkOk(vkCreateDevice(d.gpu, &deviceInfo, nullptr, &d.device),
              "vkCreateDevice")) {
        return nullptr;
    }
    vkGetDeviceQueue(d.device, d.graphicsFamily, 0, &d.graphicsQueue);
    vkGetDeviceQueue(d.device, d.presentFamily, 0, &d.presentQueue);

    // Extension entry points are not exported by the loader's static symbols.
    d.cmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(d.device, "vkCmdBeginRenderingKHR"));
    d.cmdEndRendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(d.device, "vkCmdEndRenderingKHR"));
    d.cmdPushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(d.device, "vkCmdPushDescriptorSetKHR"));
    if (!d.cmdBeginRendering || !d.cmdEndRendering || !d.cmdPushDescriptorSet) {
        LOG_ERROR("Vulkan: could not load dynamic-rendering / push-descriptor "
                  "entry points");
        return nullptr;
    }

    // --- Allocator ---
    VmaAllocatorCreateInfo allocatorInfo {};
    allocatorInfo.physicalDevice = d.gpu;
    allocatorInfo.device = d.device;
    allocatorInfo.instance = d.instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    if (!vkOk(vmaCreateAllocator(&allocatorInfo, &d.allocator),
              "vmaCreateAllocator")) {
        return nullptr;
    }

    // --- Swapchain + per-frame objects ---
    if (!d.createSwapchain()) {
        return nullptr;
    }

    VkCommandPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = d.graphicsFamily;
    if (!vkOk(vkCreateCommandPool(d.device, &poolInfo, nullptr, &d.commandPool),
              "vkCreateCommandPool")) {
        return nullptr;
    }
    VkCommandPoolCreateInfo transferPoolInfo = poolInfo;
    transferPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (!vkOk(vkCreateCommandPool(d.device, &transferPoolInfo, nullptr,
                                  &d.transferPool),
              "vkCreateCommandPool(transfer)")) {
        return nullptr;
    }

    VkCommandBufferAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = d.commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kFramesInFlight;
    if (!vkOk(vkAllocateCommandBuffers(d.device, &alloc,
                                       d.commandBuffers.data()),
              "vkAllocateCommandBuffers")) {
        return nullptr;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo sem {};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!vkOk(vkCreateSemaphore(d.device, &sem, nullptr,
                                    &d.imageAvailable[i]),
                  "vkCreateSemaphore(imageAvailable)")) {
            return nullptr;
        }
        // Created signalled so the first beginFrame does not block forever.
        VkFenceCreateInfo fence {};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!vkOk(vkCreateFence(d.device, &fence, nullptr, &d.inFlight[i]),
                  "vkCreateFence")) {
            return nullptr;
        }
    }

    // Timestamps need a non-zero timestampPeriod and a queue that supports
    // them; both are reported per-device, so caps().timerQueries follows.
    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d.gpu, &familyCount, nullptr);
    vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(d.gpu, &familyCount,
                                             families.data());
    const bool timestampsUsable =
        props.limits.timestampPeriod > 0.0f &&
        d.graphicsFamily < familyCount &&
        families[d.graphicsFamily].timestampValidBits > 0;
    if (timestampsUsable) {
        d.timestampPeriod = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo queryInfo {};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = kFramesInFlight * kTimestampsPerFrame;
        if (!vkOk(vkCreateQueryPool(d.device, &queryInfo, nullptr,
                                    &d.queryPool),
                  "vkCreateQueryPool")) {
            return nullptr;
        }
    } else {
        LOG_WARN("Vulkan: GPU timestamps unavailable on this queue");
    }

    d.cmd = std::make_unique<VulkanCommandBuffer>(d);

    // Now that resources (V2), shaders (V3) and pipelines (V4) are real, the
    // caps go on as a set — renderer systems gate on these to decide whether
    // to run, so advertising one whose path is still a no-op would be worse
    // than reporting false.
    VkSamplerCreateInfo pcfInfo {};
    pcfInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    pcfInfo.minFilter = VK_FILTER_LINEAR;
    pcfInfo.magFilter = VK_FILTER_LINEAR;
    pcfInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    pcfInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    pcfInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    pcfInfo.compareEnable = VK_TRUE;
    pcfInfo.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkOk(vkCreateSampler(d.device, &pcfInfo, nullptr, &d.pcfSampler),
         "vkCreateSampler (immutable PCF)");

    self->caps_ = { .offscreenTargets = true,
                    .textureArrays = true,
                    .hdrFormats = true,
                    .samplerObjects = true,
                    .mipmapGeneration = true,
                    .copyTexture = true,
                    .computeShaders = true,
                    .timerQueries = timestampsUsable,
                    .volumeTextures = true };

    {
        const u32 white[2] = { 0xffffffffu, 0xffffffffu };
        d.dummy2D = self->createTexture({ .width = 1, .height = 1 }, white);
        d.dummyArray = self->createTexture(
            { .width = 1, .height = 1, .arrayLayers = 2 }, white);
        d.dummy3D = self->createTexture(
            { .width = 1, .height = 1, .depth = 2 }, nullptr);
        d.dummyDepth = self->createTexture(
            { .width = 1, .height = 1, .format = TextureFormat::Depth32F },
            nullptr);
        d.dummyUniform = self->createBuffer(
            { .usage = BufferUsage::Uniform, .size = 256 }, nullptr);
        d.dummyStorage = self->createBuffer(
            { .usage = BufferUsage::Storage, .size = 256 }, nullptr);
    }

    LOG_INFO("Vulkan device ready: {} — {}x{}, {} swapchain images "
             "(V4: pipelines + draws)",
             props.deviceName, d.extent.width, d.extent.height,
             static_cast<u32>(d.images.size()));
    return self;
}

uptr<Device> createVulkanDevice(platform::Window& window) {
    return VulkanDevice::create(window);
}

} // namespace rhi

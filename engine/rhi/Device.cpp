#include "engine/rhi/Device.hpp"

#include "engine/core/Log.hpp"
#include "engine/rhi/backends/gl/GlDevice.hpp"

#ifdef MEADOWS_RHI_VULKAN
#include "engine/rhi/backends/vulkan/VulkanDevice.hpp"
#endif

namespace rhi {

bool validateTextureDesc(const TextureDesc& desc, bool hasPixels,
                         const DeviceCaps& caps) {
    const FormatTraits& traits = formatTraits(desc.format);
    if (desc.width == 0 || desc.height == 0) {
        LOG_ERROR("createTexture: zero extent");
        return false;
    }
    if (desc.depth > 1 && desc.arrayLayers > 1) {
        LOG_ERROR("createTexture: depth and arrayLayers are exclusive");
        return false;
    }
    if (traits.cooked && !caps.textureCompressionBC) {
        LOG_ERROR("createTexture: cooked format without the "
                  "textureCompressionBC capability");
        return false;
    }
    if (traits.blockCompressed && (!desc.pixelsIncludeMips || !hasPixels)) {
        LOG_ERROR("createTexture: block-compressed formats require initial "
                  "pixels with the full mip chain (pixelsIncludeMips)");
        return false;
    }
    if (desc.pixelsIncludeMips &&
        (!hasPixels || desc.format == TextureFormat::R16F)) {
        // R16F keeps its f32-to-half conversion contract, which a raw
        // mip-chain copy cannot honor.
        LOG_ERROR("createTexture: invalid pixelsIncludeMips request");
        return false;
    }
    if (hasPixels && !desc.pixelsIncludeMips && !traits.uploadable) {
        LOG_ERROR("createTexture: initial pixels are not accepted for this "
                  "format (render-target formats are created empty)");
        return false;
    }
    return true;
}

bool validateBindGroupDesc(const BindGroupDesc& desc) {
    for (const BindGroupEntry& entry : desc.entries) {
        const bool hasBuffer = entry.buffer.id != 0;
        const bool hasTexture = entry.texture.id != 0;
        if (hasBuffer == hasTexture) {
            LOG_ERROR("createBindGroup: entry {} must reference exactly one "
                      "of buffer/texture",
                      entry.binding);
            return false;
        }
        if (entry.sampler.id != 0 && !hasTexture) {
            LOG_ERROR("createBindGroup: entry {} has a sampler without a "
                      "texture",
                      entry.binding);
            return false;
        }
    }
    return true;
}

// Creates exactly the requested backend, or nullptr (logged) if it is not
// compiled in or fails to initialize. The preference/fallback chain (try
// Vulkan, fall back to GL) lives at the call site (Engine::init), because the
// SDL window surface flag is fixed at creation and must match (§2.1).
uptr<Device> Device::create(Backend backend, platform::Window& window) {
    switch (backend) {
    case Backend::OpenGL:
        return createGlDevice(window);
    case Backend::Vulkan:
#ifdef MEADOWS_RHI_VULKAN
        return createVulkanDevice(window);
#else
        LOG_ERROR("Vulkan backend requested but not compiled in "
                  "(MEADOWS_RHI_VULKAN=OFF)");
        return nullptr;
#endif
    }
    LOG_ERROR("Unknown RHI backend");
    return nullptr;
}

} // namespace rhi

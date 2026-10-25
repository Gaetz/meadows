// The single TU that emits the VMA implementation (the header only
// declares it for the other Vulkan TUs).
#define VMA_IMPLEMENTATION
#include "engine/rhi/backends/vulkan/VulkanInternal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "engine/core/Log.hpp"
#include "engine/platform/VulkanSurface.hpp"
#include "engine/platform/Window.hpp"

namespace rhi {

namespace {

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

// Referenced only when validation is compiled in — release builds skip it.
[[maybe_unused]] bool validationLayerAvailable() {
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

// Routes validation-layer messages into spdlog (they went to stdout before —
// invisible in log captures) and counts them, so a run can be judged by two
// numbers at teardown. Installed only when the validation layer is on.
VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData) {
    auto* counters = static_cast<ValidationCounters*>(userData);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        ++counters->errors;
        LOG_ERROR("[vk-validation] {}", data->pMessage);
    } else {
        ++counters->warnings;
        LOG_WARN("[vk-validation] {}", data->pMessage);
    }
    return VK_FALSE;
}

} // namespace

// --- Lifetime ----------------------------------------------------------------

VulkanDevice::VulkanDevice() : impl { std::make_unique<Impl>() } {}

VulkanDevice::~VulkanDevice() {
    if (impl->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(impl->device);
        // A frame cut short by the quit leaves its graphics / compute /
        // upload command buffers RECORDING (begun, never submitted):
        // destroying what they reference is illegal and freeing them
        // took the driver down at exit. Idle reached, nothing is pending,
        // so every pool resets cleanly.
        for (VkCommandPool pool : { impl->commandPool, impl->computePool,
                                    impl->transferPool, impl->uploadPool }) {
            if (pool != VK_NULL_HANDLE) {
                vkResetCommandPool(impl->device, pool, 0);
            }
        }
        impl->frameActive = false;
        impl->uploadRecording = false;
        impl->computeRecording = false;

        for (auto& [id, fence] : impl->fences) {
            vkDestroyFence(impl->device, fence, nullptr);
        }
        if (impl->queryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(impl->device, impl->queryPool, nullptr);
        }
        if (impl->computeQueryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(impl->device, impl->computeQueryPool, nullptr);
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
        if (impl->pipelineCache != VK_NULL_HANDLE) {
            size_t size = 0;
            vkGetPipelineCacheData(impl->device, impl->pipelineCache, &size,
                                   nullptr);
            std::string blob(size, '\0');
            if (size > 0 &&
                vkGetPipelineCacheData(impl->device, impl->pipelineCache,
                                       &size, blob.data()) == VK_SUCCESS) {
                std::ofstream out(Impl::kPipelineCacheFile, std::ios::binary);
                out.write(blob.data(), static_cast<std::streamsize>(size));
            }
            vkDestroyPipelineCache(impl->device, impl->pipelineCache, nullptr);
        }
        impl->flushPendingFrees(true);
        for (auto& [id, sampler] : impl->samplers) {
            vkDestroySampler(impl->device, sampler, nullptr);
        }
        if (impl->pcfSampler != VK_NULL_HANDLE) {
            vkDestroySampler(impl->device, impl->pcfSampler, nullptr);
        }
        for (auto& [id, tex] : impl->textures) {
            for (VkImageView v : tex.mipViews) {
                if (v != VK_NULL_HANDLE) {
                    vkDestroyImageView(impl->device, v, nullptr);
                }
            }
            vkDestroyImageView(impl->device, tex.view, nullptr);
            if (tex.defaultSampler != VK_NULL_HANDLE) {
                // The per-texture default sampler — destroyTexture
                // frees it, but textures alive at shutdown reach here.
                vkDestroySampler(impl->device, tex.defaultSampler, nullptr);
            }
            vmaDestroyImage(impl->allocator, tex.image, tex.allocation);
        }
        for (auto& [id, buf] : impl->buffers) {
            vmaDestroyBuffer(impl->allocator, buf.buffer, buf.allocation);
        }
        for (auto& ring : impl->stagingRings) {
            if (ring.buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(impl->allocator, ring.buffer,
                                 ring.allocation);
            }
        }

        for (u32 i = 0; i < kFramesInFlight; ++i) {
            vkDestroySemaphore(impl->device, impl->imageAvailable[i], nullptr);
            vkDestroyFence(impl->device, impl->inFlight[i], nullptr);
        }
        impl->destroySwapchain();
        if (impl->computePool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl->device, impl->computePool, nullptr);
        }
        if (impl->uploadPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(impl->device, impl->uploadPool, nullptr);
        }
        if (impl->timeline != VK_NULL_HANDLE) {
            vkDestroySemaphore(impl->device, impl->timeline, nullptr);
        }
        if (impl->uploadTimeline != VK_NULL_HANDLE) {
            vkDestroySemaphore(impl->device, impl->uploadTimeline, nullptr);
        }
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
        if (impl->debugMessenger != VK_NULL_HANDLE) {
            auto destroyMessenger =
                reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(
                        impl->instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyMessenger != nullptr) {
                destroyMessenger(impl->instance, impl->debugMessenger,
                                 nullptr);
            }
            // The run's verdict in two numbers.
            if (impl->validationCounts.errors +
                    impl->validationCounts.warnings >
                0) {
                LOG_WARN("Vulkan validation: {} error(s), {} warning(s) "
                         "this run",
                         impl->validationCounts.errors,
                         impl->validationCounts.warnings);
            } else {
                LOG_INFO("Vulkan validation: clean run (0 message)");
            }
        }
        if (impl->surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(impl->instance, impl->surface, nullptr);
        }
        vkDestroyInstance(impl->instance, nullptr);
    }
}

u64 VulkanDevice::nativeTextureId(TextureHandle) const {
    // Not implemented: hand ImGui a VkDescriptorSet for the offscreen
    // target (docs/RENDERING.md).
    return 0;
}

u32 VulkanDevice::validationMessageCount() const {
    return impl->validationCounts.errors + impl->validationCounts.warnings;
}

// --- Creation ----------------------------------------------------------------

namespace {

using Impl = VulkanDevice::Impl;

// The queue families one physical device offers, resolved once from a
// single vkGetPhysicalDeviceQueueFamilyProperties list. Three picks over
// that list: graphics + present (device eligibility), a compute family
// DISTINCT from graphics (async compute, docs/RENDERING.md PG3 — its own
// hardware queue; MoltenVK exposes several, each a separate
// MTLCommandQueue, which Apple GPUs schedule concurrently —
// MVK_CONFIG_SPECIALIZED_QUEUE_FAMILIES marks dedicated ones), and a
// transfer family distinct from both (the upload queue — a real DMA engine
// on PC; MoltenVK exposes generic families, a third queue there keeps the
// sync topology identical for PC validation). A pick with no distinct
// family falls back to the graphics family, which reads as "lane off".
struct QueueFamilies {
    vector<VkQueueFamilyProperties> props;
    bool graphicsFound { false };
    bool presentFound { false };
    u32 graphics { 0 };
    u32 present { 0 };
    u32 compute { 0 };
    u32 upload { 0 };

    bool complete() const { return graphicsFound && presentFound; }
    VkQueueFlags flags(u32 family) const { return props[family].queueFlags; }
    bool timestamps(u32 family) const {
        return family < props.size() && props[family].timestampValidBits > 0;
    }
};

QueueFamilies pickQueueFamilies(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
    QueueFamilies q;
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    q.props.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, q.props.data());

    // First graphics-capable and first present-capable family.
    for (u32 i = 0; i < count; ++i) {
        if (!q.graphicsFound &&
            (q.props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            q.graphics = i;
            q.graphicsFound = true;
        }
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &supported);
        if (!q.presentFound && supported) {
            q.present = i;
            q.presentFound = true;
        }
    }
    if (!q.complete()) {
        return q;
    }

    // Highest score wins, the first family in enumeration order on a tie;
    // a negative score excludes the family. Nothing eligible: graphics.
    const auto pick = [&](auto score) {
        u32 best = q.graphics;
        i32 bestScore = -1;
        for (u32 i = 0; i < count; ++i) {
            const i32 s = score(i, q.props[i].queueFlags);
            if (s > bestScore) {
                bestScore = s;
                best = i;
            }
        }
        return best;
    };
    // Async compute: compute-capable, not the graphics family; a
    // specialized (non-graphics) family over a generic one.
    q.compute = pick([&](u32 i, VkQueueFlags flags) -> i32 {
        if (i == q.graphics || (flags & VK_QUEUE_COMPUTE_BIT) == 0) {
            return -1;
        }
        return (flags & VK_QUEUE_GRAPHICS_BIT) == 0 ? 2 : 1;
    });
    // Upload: transfer-capable (graphics/compute imply it), distinct from
    // both; transfer-only first, then compute-only, then generic.
    q.upload = pick([&](u32 i, VkQueueFlags flags) -> i32 {
        if (i == q.graphics || i == q.compute ||
            (flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT |
                      VK_QUEUE_COMPUTE_BIT)) == 0) {
            return -1;
        }
        return (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0
                   ? 3
               : (flags & VK_QUEUE_GRAPHICS_BIT) == 0 ? 2
                                                      : 1;
    });
    return q;
}

// Instance, validation layer (debug builds, sync validation per run) and
// the debug-utils messenger. False when no usable loader/instance exists.
bool createInstance(Impl& d) {
    vector<const char*> extensions = platform::vulkanInstanceExtensions();
    if (extensions.empty()) {
        return false;
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
    bool syncValidation = false;
#ifndef NDEBUG
    if (validationLayerAvailable()) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        LOG_DEBUG("Vulkan: validation layer enabled");
        // Synchronization validation, opt-in PER RUN — missing-barrier
        // races are silent under standard validation (lesson learned: the
        // dynamic-UBO race rendered wrong for days with a clean layer), and
        // this mode costs several ms per frame, too slow to leave on.
        syncValidation = std::getenv("MEADOWS_VK_SYNC_VALIDATION") != nullptr;
        if (syncValidation) {
            LOG_INFO("Vulkan: SYNCHRONIZATION validation on "
                     "(MEADOWS_VK_SYNC_VALIDATION) — expect slow frames");
        }
    }
#endif
    // Debug-utils messenger: validation messages into spdlog + counted
    // (see debugUtilsCallback). Only meaningful when a layer emits them.
    const bool debugUtils =
        !layers.empty() &&
        hasExtension(availableExt, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

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
    const VkValidationFeatureEnableEXT syncEnable =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures {};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &syncEnable;
    if (syncValidation) {
        instanceInfo.pNext = &validationFeatures;
    }
    if (!vkOk(vkCreateInstance(&instanceInfo, nullptr, &d.instance),
              "vkCreateInstance")) {
        return false;
    }
    if (debugUtils) {
        auto createMessenger =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(d.instance,
                                      "vkCreateDebugUtilsMessengerEXT"));
        if (createMessenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT msgInfo {};
            msgInfo.sType =
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            msgInfo.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            msgInfo.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            msgInfo.pfnUserCallback = debugUtilsCallback;
            msgInfo.pUserData = &d.validationCounts;
            createMessenger(d.instance, &msgInfo, nullptr, &d.debugMessenger);
        }
    }
    return true;
}

// Best GPU that can present to the surface with a swapchain (discrete
// preferred; on the M1 there is only the integrated one), together with
// its queue families. The queue flags of the compute/upload picks are
// recorded on the device whether or not the lanes end up created.
bool pickPhysicalDevice(Impl& d, QueueFamilies& families) {
    u32 gpuCount = 0;
    vkEnumeratePhysicalDevices(d.instance, &gpuCount, nullptr);
    if (gpuCount == 0) {
        LOG_ERROR("Vulkan: no physical device found");
        return false;
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
        QueueFamilies candidateFamilies =
            pickQueueFamilies(candidate, d.surface);
        if (!candidateFamilies.complete()) {
            continue;
        }
        VkPhysicalDeviceProperties candidateProps {};
        vkGetPhysicalDeviceProperties(candidate, &candidateProps);
        const i32 score =
            candidateProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                ? 2
                : 1;
        if (score > bestScore) {
            bestScore = score;
            d.gpu = candidate;
            families = std::move(candidateFamilies);
        }
    }
    if (d.gpu == VK_NULL_HANDLE) {
        LOG_ERROR("Vulkan: no device with graphics + present + swapchain");
        return false;
    }
    d.graphicsFamily = families.graphics;
    d.presentFamily = families.present;
    d.computeQueueFlags = families.flags(families.compute);
    d.uploadQueueFlags = families.flags(families.upload);
    return true;
}

// Logical device: required extensions, the opt-in features the GPU has,
// one queue per distinct family in use. Reports which extra lanes the
// device was created for (async compute needs timeline semaphores and a
// distinct compute family; the upload lane rides the same timeline
// feature and needs a family distinct from both).
bool createLogicalDevice(Impl& d, const QueueFamilies& q,
                         const VkPhysicalDeviceFeatures& features,
                         bool& wantAsyncCompute, bool& wantUploadQueue) {
    // Timeline semaphores (core 1.2, an enable-gated feature) are the
    // cross-queue sync primitive; without them async compute stays off.
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature {};
    timelineFeature.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    {
        VkPhysicalDeviceFeatures2 timelineQuery {};
        timelineQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        timelineQuery.pNext = &timelineFeature;
        vkGetPhysicalDeviceFeatures2(d.gpu, &timelineQuery);
    }
    wantAsyncCompute = q.compute != q.graphics &&
                       timelineFeature.timelineSemaphore == VK_TRUE;

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
    // Both verified present on MoltenVK/M1 (docs/RENDERING.md). Dynamic rendering
    // removes VkRenderPass/VkFramebuffer entirely; push descriptors let
    // setBindGroup write through the bound pipeline's layout, which is what
    // makes the RHI's shader-agnostic BindGroupDesc expressible at all.
    if (!hasExtension(devExt, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ||
        !hasExtension(devExt, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        LOG_ERROR("Vulkan: device lacks dynamic_rendering and/or "
                  "push_descriptor — both are required by this backend");
        return false;
    }
    deviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    deviceExtensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

    // Only anisotropy, multi-draw-indirect and BC compression are requested,
    // and only if the GPU has them — samplers fall back to isotropic
    // filtering; indirect draws fall back to a per-draw loop; without BC the
    // cooked-texture consumers keep their procedural fallback
    // (caps.textureCompressionBC).
    VkPhysicalDeviceFeatures enabled {};
    enabled.samplerAnisotropy = features.samplerAnisotropy;
    enabled.multiDrawIndirect = features.multiDrawIndirect;
    enabled.textureCompressionBC = features.textureCompressionBC;
    d.multiDrawIndirect = features.multiDrawIndirect == VK_TRUE;
    d.textureCompressionBC = features.textureCompressionBC == VK_TRUE;

    const f32 priority = 1.0f;
    vector<VkDeviceQueueCreateInfo> queueInfos;
    VkDeviceQueueCreateInfo graphicsQueueInfo {};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = q.graphics;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &priority;
    queueInfos.push_back(graphicsQueueInfo);
    if (q.present != q.graphics) {
        VkDeviceQueueCreateInfo presentQueueInfo = graphicsQueueInfo;
        presentQueueInfo.queueFamilyIndex = q.present;
        queueInfos.push_back(presentQueueInfo);
    }
    if (wantAsyncCompute && q.compute != q.present) {
        VkDeviceQueueCreateInfo computeQueueInfo = graphicsQueueInfo;
        computeQueueInfo.queueFamilyIndex = q.compute;
        queueInfos.push_back(computeQueueInfo);
    }
    // Gated on the same timeline feature as async compute — the upload
    // chain is pure timeline sync.
    wantUploadQueue = wantAsyncCompute && q.upload != q.graphics &&
                      q.upload != q.compute;
    if (wantUploadQueue && q.upload != q.present) {
        VkDeviceQueueCreateInfo uploadQueueInfo = graphicsQueueInfo;
        uploadQueueInfo.queueFamilyIndex = q.upload;
        queueInfos.push_back(uploadQueueInfo);
    }

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering {};
    dynamicRendering.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    dynamicRendering.dynamicRendering = VK_TRUE;

    VkDeviceCreateInfo deviceInfo {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &dynamicRendering;
    if (wantAsyncCompute) {
        timelineFeature.pNext = const_cast<void*>(deviceInfo.pNext);
        deviceInfo.pNext = &timelineFeature;
    }
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
        return false;
    }
    vkGetDeviceQueue(d.device, q.graphics, 0, &d.graphicsQueue);
    vkGetDeviceQueue(d.device, q.present, 0, &d.presentQueue);
    return true;
}

// Async-compute lane: its queue, command pool, the shared timeline
// semaphore and one command buffer per frame slot. Any failure leaves the
// lane off (asyncComputeAvailable false) — the frame then runs single-queue.
void createComputeLane(Impl& d, const QueueFamilies& q) {
    d.computeFamily = q.compute;
    vkGetDeviceQueue(d.device, q.compute, 0, &d.computeQueue);
    VkCommandPoolCreateInfo computePoolInfo {};
    computePoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    computePoolInfo.flags =
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    computePoolInfo.queueFamilyIndex = q.compute;
    VkSemaphoreTypeCreateInfo timelineType {};
    timelineType.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineType.initialValue = 0;
    VkSemaphoreCreateInfo timelineSem {};
    timelineSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timelineSem.pNext = &timelineType;
    if (vkOk(vkCreateCommandPool(d.device, &computePoolInfo, nullptr,
                                 &d.computePool),
             "vkCreateCommandPool(compute)") &&
        vkOk(vkCreateSemaphore(d.device, &timelineSem, nullptr,
                               &d.timeline),
             "vkCreateSemaphore(timeline)")) {
        VkCommandBufferAllocateInfo computeAlloc {};
        computeAlloc.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        computeAlloc.commandPool = d.computePool;
        computeAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        computeAlloc.commandBufferCount = kFramesInFlight;
        if (vkOk(vkAllocateCommandBuffers(d.device, &computeAlloc,
                                          d.computeCbs.data()),
                 "vkAllocateCommandBuffers(compute)")) {
            d.computeCmd = std::make_unique<VulkanCommandBuffer>(d);
            d.asyncComputeAvailable = true;
            LOG_INFO("Vulkan: async compute on queue family {} "
                     "(graphics on {})",
                     q.compute, d.graphicsFamily);
        }
    }
}

// Upload lane: its queue, command pool, its OWN timeline semaphore (Impl
// notes) and one command buffer per frame slot. Failure leaves the lane off.
void createUploadLane(Impl& d, const QueueFamilies& q) {
    d.transferFamily = q.upload;
    vkGetDeviceQueue(d.device, q.upload, 0, &d.transferQueue);
    VkCommandPoolCreateInfo uploadPoolInfo {};
    uploadPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    uploadPoolInfo.queueFamilyIndex = q.upload;
    VkSemaphoreTypeCreateInfo uploadTimelineType {};
    uploadTimelineType.sType =
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    uploadTimelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    uploadTimelineType.initialValue = 0;
    VkSemaphoreCreateInfo uploadTimelineSem {};
    uploadTimelineSem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    uploadTimelineSem.pNext = &uploadTimelineType;
    if (vkOk(vkCreateCommandPool(d.device, &uploadPoolInfo, nullptr,
                                 &d.uploadPool),
             "vkCreateCommandPool(upload)") &&
        vkOk(vkCreateSemaphore(d.device, &uploadTimelineSem, nullptr,
                               &d.uploadTimeline),
             "vkCreateSemaphore(uploadTimeline)")) {
        VkCommandBufferAllocateInfo uploadAlloc {};
        uploadAlloc.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        uploadAlloc.commandPool = d.uploadPool;
        uploadAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        uploadAlloc.commandBufferCount = kFramesInFlight;
        if (vkOk(vkAllocateCommandBuffers(d.device, &uploadAlloc,
                                          d.uploadCbs.data()),
                 "vkAllocateCommandBuffers(upload)")) {
            d.uploadQueueAvailable = true;
            LOG_INFO("Vulkan: upload queue on family {} (compute on {}, "
                     "graphics on {})",
                     q.upload, q.compute, d.graphicsFamily);
        }
    }
}

// Extension entry points are not exported by the loader's static symbols.
bool loadExtensionEntryPoints(Impl& d) {
    d.cmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(d.device, "vkCmdBeginRenderingKHR"));
    d.cmdEndRendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(d.device, "vkCmdEndRenderingKHR"));
    d.cmdPushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(d.device, "vkCmdPushDescriptorSetKHR"));
    if (!d.cmdBeginRendering || !d.cmdEndRendering || !d.cmdPushDescriptorSet) {
        LOG_ERROR("Vulkan: could not load dynamic-rendering / push-descriptor "
                  "entry points");
        return false;
    }
    return true;
}

bool createAllocator(Impl& d) {
    VmaAllocatorCreateInfo allocatorInfo {};
    allocatorInfo.physicalDevice = d.gpu;
    allocatorInfo.device = d.device;
    allocatorInfo.instance = d.instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    return vkOk(vmaCreateAllocator(&allocatorInfo, &d.allocator),
                "vmaCreateAllocator");
}

// Swapchain, the graphics command pool + transient transfer pool, and the
// per-frame-slot command buffers, acquire semaphores and fences.
bool createFrameObjects(Impl& d) {
    if (!d.createSwapchain()) {
        return false;
    }

    VkCommandPoolCreateInfo poolInfo {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = d.graphicsFamily;
    if (!vkOk(vkCreateCommandPool(d.device, &poolInfo, nullptr, &d.commandPool),
              "vkCreateCommandPool")) {
        return false;
    }
    VkCommandPoolCreateInfo transferPoolInfo = poolInfo;
    transferPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (!vkOk(vkCreateCommandPool(d.device, &transferPoolInfo, nullptr,
                                  &d.transferPool),
              "vkCreateCommandPool(transfer)")) {
        return false;
    }

    VkCommandBufferAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = d.commandPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kFramesInFlight;
    if (!vkOk(vkAllocateCommandBuffers(d.device, &alloc,
                                       d.commandBuffers.data()),
              "vkAllocateCommandBuffers")) {
        return false;
    }

    for (u32 i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo sem {};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (!vkOk(vkCreateSemaphore(d.device, &sem, nullptr,
                                    &d.imageAvailable[i]),
                  "vkCreateSemaphore(imageAvailable)")) {
            return false;
        }
        // Created signalled so the first beginFrame does not block forever.
        VkFenceCreateInfo fence {};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!vkOk(vkCreateFence(d.device, &fence, nullptr, &d.inFlight[i]),
                  "vkCreateFence")) {
            return false;
        }
    }
    return true;
}

// Timestamps need a non-zero timestampPeriod and a queue that supports
// them; both are reported per-device, so caps().timerQueries follows.
// False only when the graphics pool itself cannot be created.
bool createQueryPools(Impl& d, const QueueFamilies& q,
                      const VkPhysicalDeviceProperties& props,
                      bool& timestampsUsable) {
    timestampsUsable = props.limits.timestampPeriod > 0.0f &&
                       q.timestamps(d.graphicsFamily);
    if (!timestampsUsable) {
        LOG_WARN("Vulkan: GPU timestamps unavailable on this queue");
        return true;
    }
    d.timestampPeriod = props.limits.timestampPeriod;
    VkQueryPoolCreateInfo queryInfo {};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = kFramesInFlight * kTimestampsPerFrame;
    if (!vkOk(vkCreateQueryPool(d.device, &queryInfo, nullptr,
                                &d.queryPool),
              "vkCreateQueryPool")) {
        return false;
    }
    // Per-queue timestamps: the async compute stream gets its own pool
    // (scopes recorded there measured nothing on the graphics pool).
    if (d.asyncComputeAvailable && q.timestamps(d.computeFamily)) {
        vkOk(vkCreateQueryPool(d.device, &queryInfo, nullptr,
                               &d.computeQueryPool),
             "vkCreateQueryPool(compute)");
    }
    return true;
}

// Seeds the pipeline cache from the previous run's blob (Impl notes on
// why it is persisted). Vulkan validates the blob header itself
// (device/driver UUID): a stale or foreign file is simply ignored.
void loadPipelineCache(Impl& d) {
    std::ifstream in(Impl::kPipelineCacheFile,
                     std::ios::binary | std::ios::ate);
    std::string blob;
    if (in) {
        blob.resize(static_cast<size_t>(in.tellg()));
        in.seekg(0);
        in.read(blob.data(), static_cast<std::streamsize>(blob.size()));
    }
    VkPipelineCacheCreateInfo cacheInfo {};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheInfo.initialDataSize = blob.size();
    cacheInfo.pInitialData = blob.empty() ? nullptr : blob.data();
    vkOk(vkCreatePipelineCache(d.device, &cacheInfo, nullptr,
                               &d.pipelineCache),
         "vkCreatePipelineCache");
}

// The device-owned comparison sampler baked into layouts as IMMUTABLE for
// every sampler*Shadow binding (Impl::pcfSampler).
void createPcfSampler(Impl& d) {
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
}

// The stand-ins pushGroup/bindMissingDummies fall back on (Impl::dummy*).
// Created through the public API once the device is complete.
void createDummyResources(VulkanDevice& device, Impl& d) {
    const u32 white[2] = { 0xffffffffu, 0xffffffffu };
    d.dummy2D = device.createTexture({ .width = 1, .height = 1 }, white);
    d.dummyArray = device.createTexture(
        { .width = 1, .height = 1, .arrayLayers = 2 }, white);
    d.dummy3D = device.createTexture(
        { .width = 1, .height = 1, .depth = 2 }, nullptr);
    d.dummyDepth = device.createTexture(
        { .width = 1, .height = 1, .format = TextureFormat::Depth32F },
        nullptr);
    d.dummyUniform = device.createBuffer(
        { .usage = BufferUsage::Uniform, .size = 256 }, nullptr);
    d.dummyStorage = device.createBuffer(
        { .usage = BufferUsage::Storage, .size = 256 }, nullptr);
}

} // namespace

uptr<VulkanDevice> VulkanDevice::create(platform::Window& window) {
    auto self = uptr<VulkanDevice> { new VulkanDevice() };
    Impl& d = *self->impl;
    d.window = &window;

    if (!createInstance(d)) {
        return nullptr;
    }
    d.surface = reinterpret_cast<VkSurfaceKHR>(
        platform::createVulkanSurface(window, d.instance));
    if (d.surface == VK_NULL_HANDLE) {
        return nullptr;
    }
    QueueFamilies families;
    if (!pickPhysicalDevice(d, families)) {
        return nullptr;
    }
    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(d.gpu, &props);
    VkPhysicalDeviceFeatures features {};
    vkGetPhysicalDeviceFeatures(d.gpu, &features);

    bool wantAsyncCompute = false;
    bool wantUploadQueue = false;
    if (!createLogicalDevice(d, families, features, wantAsyncCompute,
                             wantUploadQueue)) {
        return nullptr;
    }
    if (wantAsyncCompute) {
        createComputeLane(d, families);
    }
    if (wantUploadQueue && d.asyncComputeAvailable) {
        createUploadLane(d, families);
    }
    if (!loadExtensionEntryPoints(d) || !createAllocator(d) ||
        !createFrameObjects(d)) {
        return nullptr;
    }
    bool timestampsUsable = false;
    if (!createQueryPools(d, families, props, timestampsUsable)) {
        return nullptr;
    }

    d.cmd = std::make_unique<VulkanCommandBuffer>(d);

    loadPipelineCache(d);
    createPcfSampler(d);

    // Now that resources, shaders and pipelines are real, the
    // caps go on as a set — renderer systems gate on these to decide whether
    // to run, so advertising one whose path is still a no-op would be worse
    // than reporting false.
    self->caps_ = { .offscreenTargets = true,
                    .textureArrays = true,
                    .hdrFormats = true,
                    .samplerObjects = true,
                    .mipmapGeneration = true,
                    .copyTexture = true,
                    .computeShaders = true,
                    .timerQueries = timestampsUsable,
                    .volumeTextures = true,
                    .asyncCompute = d.asyncComputeAvailable,
                    .multiDrawIndirect = d.multiDrawIndirect,
                    .textureCompressionBC = d.textureCompressionBC };

    createDummyResources(*self, d);

    LOG_INFO("Vulkan device ready: {} — {}x{}, {} swapchain images",
             props.deviceName, d.extent.width, d.extent.height,
             static_cast<u32>(d.images.size()));
    return self;
}

uptr<Device> createVulkanDevice(platform::Window& window) {
    return VulkanDevice::create(window);
}

} // namespace rhi

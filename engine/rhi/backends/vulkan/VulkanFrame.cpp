#include "engine/rhi/backends/vulkan/VulkanInternal.hpp"

#include <algorithm>
#include <array>

#include "engine/core/Log.hpp"
#include "engine/platform/Window.hpp"

namespace rhi {

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

// --- Frame -------------------------------------------------------------------

CommandBuffer& VulkanDevice::beginFrame() {
    Impl& d = *impl;
    d.frameActive = false;

    vkWaitForFences(d.device, 1, &d.inFlight[d.frame], VK_TRUE, UINT64_MAX);
    // The graphics fence proves nothing about the compute queue: wait
    // (timeline) for this slot's last compute submission too, BEFORE the
    // parked frees run — a staging consumed by that chain must not be
    // destroyed while it still executes (sync-validation caught it).
    if (d.asyncComputeAvailable && d.computeSlotValue[d.frame] != 0) {
        VkSemaphoreWaitInfo wait {};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &d.timeline;
        wait.pValues = &d.computeSlotValue[d.frame];
        vkOk(vkWaitSemaphores(d.device, &wait, UINT64_MAX),
             "vkWaitSemaphores(compute slot)");
    }
    // Same proof for the upload queue: the slot's staging-ring slices and
    // parked frees must outlive its last transfer submission.
    if (d.uploadQueueAvailable && d.uploadSlotValue[d.frame] != 0) {
        VkSemaphoreWaitInfo wait {};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &d.uploadTimeline;
        wait.pValues = &d.uploadSlotValue[d.frame];
        vkOk(vkWaitSemaphores(d.device, &wait, UINT64_MAX),
             "vkWaitSemaphores(upload slot)");
    }
    d.frameCounter++;
    d.flushPendingFrees(false); // slot fence passed: old parked frees are safe
    d.stagingRings[d.frame].head = 0; // same proof covers the slot's ring

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
    // correct here — the previous contents are not preserved. NOT the generic
    // transitionLayout: its UNDEFINED source stage is TOP_OF_PIPE, which does
    // not chain to the acquire-semaphore wait (pWaitDstStageMask =
    // COLOR_ATTACHMENT_OUTPUT at submit) — the layout write then races the
    // presentation engine's read (WRITE_AFTER_READ vs
    // vkAcquireNextImageKHR, caught by sync validation).
    // Matching srcStage chains the barrier after the wait.
    {
        VkImageMemoryBarrier barrier {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = d.images[d.imageIndex];
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(d.commandBuffers[d.frame],
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Recycle this slot's timestamp region: reset is illegal inside a render
    // pass, so it has to happen here, before any beginRenderPass. First,
    // harvest every still-pending timestamp of this slot into its handle —
    // the fence wait above proves the GPU retired the slot, so the results
    // are available NOW and would be wiped by the reset (this was a 1-in-5
    // vksmoke flake: a caller polling one frame too late lost the value).
    if (d.queryPool != VK_NULL_HANDLE) {
        for (auto& [id, pending] : d.timestamps) {
            if (pending.cached || pending.frameSlot != d.frame) {
                continue;
            }
            // Both pools harvest here: the fence wait covers the graphics
            // stream, the timeline wait above covers the compute stream.
            const u32 liveGeneration =
                pending.onComputePool ? d.computeRegionGeneration[d.frame]
                                      : d.regionGeneration[d.frame];
            if (pending.generation != liveGeneration) {
                continue;
            }
            const VkQueryPool pool =
                pending.onComputePool ? d.computeQueryPool : d.queryPool;
            u64 ticks = 0;
            if (vkGetQueryPoolResults(d.device, pool, pending.query, 1,
                                      sizeof(ticks), &ticks, sizeof(ticks),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                pending.cachedTicks = ticks;
                pending.cached = true;
            }
        }
        vkCmdResetQueryPool(d.commandBuffers[d.frame], d.queryPool,
                            d.frame * kTimestampsPerFrame, kTimestampsPerFrame);
        ++d.regionGeneration[d.frame];
        d.timestampCursor = 0;
        // The compute region resets at the top of the compute cb
        // (asyncComputeCmd) — same-queue ordering with its writes.
    }

    d.cmd->begin(d.commandBuffers[d.frame], d.imageViews[d.imageIndex],
                 d.colorFormat, d.extent);
    d.frameActive = true;
    return *d.cmd;
}

CommandBuffer* VulkanDevice::asyncComputeCmd() {
    Impl& d = *impl;
    if (!d.asyncComputeAvailable || !d.frameActive) {
        return nullptr;
    }
    if (!d.computeRecording) {
        // Slot reuse is safe: beginFrame already waited this slot's last
        // compute value on the timeline.
        VkCommandBufferBeginInfo begin {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!vkOk(vkBeginCommandBuffer(d.computeCbs[d.frame], &begin),
                  "vkBeginCommandBuffer(compute)")) {
            return nullptr;
        }
        d.computeCmd->begin(d.computeCbs[d.frame], VK_NULL_HANDLE,
                            VK_FORMAT_UNDEFINED, {});
        d.computeRecording = true;
        if (d.computeQueryPool != VK_NULL_HANDLE) {
            // Recycle this slot's compute-timestamp region. Safe NOW: the
            // beginFrame timeline wait proved the slot's last compute
            // submission retired (and harvested its still-pending results).
            vkCmdResetQueryPool(d.computeCbs[d.frame], d.computeQueryPool,
                                d.frame * kTimestampsPerFrame,
                                kTimestampsPerFrame);
            ++d.computeRegionGeneration[d.frame];
            d.computeTimestampCursor = 0;
        }
    }
    d.computeRouting = true;
    return d.computeCmd.get();
}

void VulkanDevice::endAsyncCompute() {
    impl->computeRouting = false;
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

    const bool computePending = d.computeRecording;

    // Upload submit FIRST: the graphics submit below waits its value, so
    // every pass of this frame reads the uploaded data. WAR: waits last
    // frame's graphics value at TRANSFER (the previous frame may still be
    // reading a buffer being rewritten). Own timeline: see Impl notes.
    if (d.uploadRecording) {
        vkOk(vkEndCommandBuffer(d.uploadCbs[d.frame]),
             "vkEndCommandBuffer(upload)");
        const u64 uploadDone = ++d.uploadNext;
        const VkPipelineStageFlags uploadWaitStage =
            VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkTimelineSemaphoreSubmitInfo uploadTimelineInfo {};
        uploadTimelineInfo.sType =
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        uploadTimelineInfo.waitSemaphoreValueCount =
            d.gfxDoneValue != 0 ? 1 : 0;
        uploadTimelineInfo.pWaitSemaphoreValues = &d.gfxDoneValue;
        uploadTimelineInfo.signalSemaphoreValueCount = 1;
        uploadTimelineInfo.pSignalSemaphoreValues = &uploadDone;
        VkSubmitInfo uploadSubmit {};
        uploadSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        uploadSubmit.pNext = &uploadTimelineInfo;
        uploadSubmit.waitSemaphoreCount = d.gfxDoneValue != 0 ? 1 : 0;
        uploadSubmit.pWaitSemaphores = &d.timeline;
        uploadSubmit.pWaitDstStageMask = &uploadWaitStage;
        uploadSubmit.commandBufferCount = 1;
        uploadSubmit.pCommandBuffers = &d.uploadCbs[d.frame];
        uploadSubmit.signalSemaphoreCount = 1;
        uploadSubmit.pSignalSemaphores = &d.uploadTimeline;
        if (vkOk(vkQueueSubmit(d.transferQueue, 1, &uploadSubmit,
                               VK_NULL_HANDLE),
                 "vkQueueSubmit(upload)")) {
            d.uploadDoneValue = uploadDone;
            d.uploadSlotValue[d.frame] = uploadDone;
        }
        d.uploadRecording = false;
    }

    // Graphics submit. With async compute in flight (PG3): waits LAST
    // frame's compute value at the CONSUMER stages only (fragment,
    // compute dispatches, depth writes — the CSM rewrite is a WAR
    // against the chain's shadow-map reads); vertex work and transfers
    // run concurrently with the still-running chain. Signals its own
    // timeline value so this frame's compute can wait the graphics work
    // it reads (CSM, staging copies). With uploads pending, a third wait
    // (uploadTimeline) covers every stage that reads uploaded buffers.
    const u64 gfxDone = ++d.timelineNext;
    std::array<VkSemaphore, 3> waitSems { d.imageAvailable[d.frame],
                                          d.timeline, d.uploadTimeline };
    std::array<u64, 3> waitValues { 0, d.computeDoneValue,
                                    d.uploadDoneValue };
    std::array<VkPipelineStageFlags, 3> waitStages {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
    };
    std::array<VkSemaphore, 2> signalSems { d.renderFinished[d.imageIndex],
                                            d.timeline };
    std::array<u64, 2> signalValues { 0, gfxDone };
    // Signal the timeline on EVERY graphics submit once it exists:
    // gfxDoneValue is handed to the upload queue as a wait value, so a
    // submit that skipped the signal would deadlock the first upload.
    // A wait on value 0 is trivially satisfied, so the 3-entry form is
    // safe even before the first compute submission.
    const bool useTimeline = d.asyncComputeAvailable;
    const bool waitUpload =
        d.uploadQueueAvailable && d.uploadDoneValue != 0;
    const u32 waitCount = waitUpload ? 3 : (useTimeline ? 2 : 1);
    VkTimelineSemaphoreSubmitInfo timelineInfo {};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount = waitCount;
    timelineInfo.pWaitSemaphoreValues = waitValues.data();
    timelineInfo.signalSemaphoreValueCount = 2;
    timelineInfo.pSignalSemaphoreValues = signalValues.data();
    VkSubmitInfo submit {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waitSems.data();
    submit.pWaitDstStageMask = waitStages.data();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = useTimeline || waitUpload ? 2 : 1;
    submit.pSignalSemaphores = signalSems.data();
    if (useTimeline || waitUpload) {
        submit.pNext = &timelineInfo;
    }
    if (!vkOk(vkQueueSubmit(d.graphicsQueue, 1, &submit, d.inFlight[d.frame]),
              "vkQueueSubmit")) {
        d.frameActive = false;
        return;
    }
    d.gfxDoneValue = gfxDone;

    // Compute submit: waits this frame's graphics (CSM + staging copies
    // it reads), signals its value for the NEXT graphics submit.
    if (computePending) {
        vkOk(vkEndCommandBuffer(d.computeCbs[d.frame]),
             "vkEndCommandBuffer(compute)");
        const u64 computeDone = ++d.timelineNext;
        const VkPipelineStageFlags computeWaitStage =
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkTimelineSemaphoreSubmitInfo computeTimeline {};
        computeTimeline.sType =
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        computeTimeline.waitSemaphoreValueCount = 1;
        computeTimeline.pWaitSemaphoreValues = &gfxDone;
        computeTimeline.signalSemaphoreValueCount = 1;
        computeTimeline.pSignalSemaphoreValues = &computeDone;
        VkSubmitInfo computeSubmit {};
        computeSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        computeSubmit.pNext = &computeTimeline;
        computeSubmit.waitSemaphoreCount = 1;
        computeSubmit.pWaitSemaphores = &d.timeline;
        computeSubmit.pWaitDstStageMask = &computeWaitStage;
        computeSubmit.commandBufferCount = 1;
        computeSubmit.pCommandBuffers = &d.computeCbs[d.frame];
        computeSubmit.signalSemaphoreCount = 1;
        computeSubmit.pSignalSemaphores = &d.timeline;
        if (vkOk(vkQueueSubmit(d.computeQueue, 1, &computeSubmit,
                               VK_NULL_HANDLE),
                 "vkQueueSubmit(compute)")) {
            d.computeDoneValue = computeDone;
            d.computeSlotValue[d.frame] = computeDone;
        }
        d.computeRecording = false;
    }
    d.computeRouting = false;

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

} // namespace rhi

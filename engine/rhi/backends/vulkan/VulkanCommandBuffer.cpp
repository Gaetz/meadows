#include "engine/rhi/backends/vulkan/VulkanInternal.hpp"

#include "engine/core/Log.hpp"

namespace rhi {

// --- Barriers ---------------------------------------------------------------

namespace {

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
        // READ too: a loadOp LOAD reads the attachment (the load raced
        // the transition when only WRITE was in scope).
        access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        // LATE too: depth writes/stores happen in BOTH fragment-test
        // stages — EARLY alone let the end-of-pass transition race the
        // store (WAW on the CSM depth). READ for LOAD.
        access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
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

} // namespace

VkPipelineStageFlags clampStagesToQueue(VkPipelineStageFlags stages,
                                        VkQueueFlags caps) {
    constexpr VkPipelineStageFlags kGraphicsOnly =
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    VkPipelineStageFlags out = stages;
    if ((caps & VK_QUEUE_GRAPHICS_BIT) == 0) {
        out &= ~kGraphicsOnly;
        // Indirect reads are compute-legal too; the mask above dropped
        // them with the graphics set.
        if ((caps & VK_QUEUE_COMPUTE_BIT) != 0 &&
            (stages & VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT) != 0) {
            out |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        }
    }
    if ((caps & VK_QUEUE_COMPUTE_BIT) == 0) {
        out &= ~static_cast<VkPipelineStageFlags>(
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    return out == 0 ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : out;
}

void transitionLayout(VkCommandBuffer cb, VkImage image,
                      VkImageAspectFlags aspect, u32 baseMip, u32 mipCount,
                      u32 layerCount, VkImageLayout oldLayout,
                      VkImageLayout newLayout,
                      VkQueueFlags queueCaps) {
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
    // A stage the family lacks takes its access bits with it (a shader
    // read on a transfer queue is meaningless): the cross-queue
    // visibility rides the timeline semaphore, not this barrier.
    constexpr VkAccessFlags kAnyStageAccess =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    const VkPipelineStageFlags srcClamped = clampStagesToQueue(srcStage, queueCaps);
    const VkPipelineStageFlags dstClamped = clampStagesToQueue(dstStage, queueCaps);
    if (srcClamped != srcStage) {
        barrier.srcAccessMask &= kAnyStageAccess;
    }
    if (dstClamped != dstStage) {
        barrier.dstAccessMask &= kAnyStageAccess;
    }
    srcStage = srcClamped;
    dstStage = dstClamped;

    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

// --- Command buffer ---------------------------------------------------------

namespace {

VkAttachmentLoadOp toVkLoadOp(LoadOp op) {
    switch (op) {
    case LoadOp::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case LoadOp::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

VkAttachmentStoreOp toVkStoreOp(StoreOp op) {
    return op == StoreOp::DontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                   : VK_ATTACHMENT_STORE_OP_STORE;
}

} // namespace

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
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, queueCaps());
                tex->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            VkRenderingAttachmentInfo color {};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = target_->colorViews[i];
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = toVkLoadOp(desc.loadOp);
            color.storeOp = toVkStoreOp(desc.storeOp);
            color.clearValue = clearColor;
            colorAttachments.push_back(color);
        }
        if (target_->depthView != VK_NULL_HANDLE) {
            VulkanTexture* tex = d_->findTexture(target_->depthTexture);
            if (tex != nullptr) {
                transitionLayout(
                    cb_, tex->image, tex->aspect, 0, tex->mipLevels,
                    tex->arrayLayers, tex->layout,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, queueCaps());
                tex->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }
            depthAttachment.sType =
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = target_->depthView;
            depthAttachment.imageLayout =
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = toVkLoadOp(desc.depthLoadOp);
            depthAttachment.storeOp = toVkStoreOp(desc.depthStoreOp);
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
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queueCaps());
                tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        VulkanTexture* depth = d_->findTexture(target_->depthTexture);
        if (depth != nullptr) {
            transitionLayout(cb_, depth->image, depth->aspect, 0,
                             depth->mipLevels, depth->arrayLayers,
                             depth->layout,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queueCaps());
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
    } else {
        // A slot beyond the replay array would bind now and silently
        // VANISH on the next setPipeline — the failure mode that made the
        // GI invisible. Scream instead.
        LOG_ERROR("Vulkan setBindGroup: slot {} exceeds the replay "
                  "capacity ({}) — its descriptors will drop on the next "
                  "pipeline change",
                  index, boundGroups_.size());
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
    // no layout to match against (docs/RENDERING.md, push descriptors).
    vector<VkWriteDescriptorSet>& writes = scratchWrites_;
    vector<VkDescriptorBufferInfo>& bufferInfos = scratchBufferInfos_;
    vector<VkDescriptorImageInfo>& imageInfos = scratchImageInfos_;
    writes.clear();
    bufferInfos.clear();
    imageInfos.clear();
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
                                     tex->layout, VK_IMAGE_LAYOUT_GENERAL, queueCaps());
                    tex->layout = VK_IMAGE_LAYOUT_GENERAL;
                }
                image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                // ONE mip exactly (GL: glBindImageTexture(level)): with the
                // whole-texture view, writes land in mip 0 whatever
                // entry.imageMip says — the Hi-Z pyramid bug.
                image.imageView = d_->textureMipView(*tex, entry.imageMip);
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
    vector<VkWriteDescriptorSet>& writes = scratchWrites_;
    vector<VkDescriptorBufferInfo>& bufferInfos = scratchBufferInfos_;
    vector<VkDescriptorImageInfo>& imageInfos = scratchImageInfos_;
    const size_t count = boundPipeline_->resources.size();
    writes.clear();
    bufferInfos.clear();
    imageInfos.clear();
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
                                     tex->layout, VK_IMAGE_LAYOUT_GENERAL, queueCaps());
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

void VulkanCommandBuffer::drawIndexedIndirect(BufferHandle args, u64 offset,
                                              u32 drawCount, u32 stride) {
    VulkanBuffer* buffer = d_->findBuffer(args);
    if (cb_ == VK_NULL_HANDLE || buffer == nullptr || drawCount == 0) {
        return;
    }
    bindMissingDummies();
    if (d_->multiDrawIndirect || drawCount == 1) {
        vkCmdDrawIndexedIndirect(cb_, buffer->buffer, offset, drawCount,
                                 stride);
        return;
    }
    // Feature absent: the per-draw loop is still GPU-driven (no readback),
    // just more submission records.
    for (u32 i = 0; i < drawCount; ++i) {
        vkCmdDrawIndexedIndirect(cb_, buffer->buffer,
                                 offset + u64(i) * stride, 1, stride);
    }
}

void VulkanCommandBuffer::dispatch(u32 groupsX, u32 groupsY, u32 groupsZ) {
    if (cb_ != VK_NULL_HANDLE) {
        bindMissingDummies();
        vkCmdDispatch(cb_, groupsX, groupsY, groupsZ);
    }
}

void VulkanCommandBuffer::memoryBarrier(u32 dst) {
    if (cb_ == VK_NULL_HANDLE) {
        return;
    }
    // The destination scope is the whole point (docs/RENDERING.md PG1):
    // COMPUTE -> ALL_COMMANDS at every internal pass boundary is what kept
    // the frame strictly serial. Scoped destinations leave later,
    // independent stages free to overlap the compute that wrote nothing
    // they read.
    VkPipelineStageFlags dstStages = 0;
    VkAccessFlags dstAccess = 0;
    if (dst == BarrierStage_All) {
        dstStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                    VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                    VK_ACCESS_TRANSFER_READ_BIT;
    } else {
        if (dst & BarrierStage_Compute) {
            dstStages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            dstAccess |= VK_ACCESS_SHADER_READ_BIT |
                         VK_ACCESS_UNIFORM_READ_BIT;
        }
        if (dst & BarrierStage_Fragment) {
            dstStages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dstAccess |= VK_ACCESS_SHADER_READ_BIT |
                         VK_ACCESS_UNIFORM_READ_BIT;
        }
        if (dst & BarrierStage_Vertex) {
            dstStages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
            dstAccess |= VK_ACCESS_SHADER_READ_BIT |
                         VK_ACCESS_UNIFORM_READ_BIT;
        }
        if (dst & BarrierStage_Transfer) {
            dstStages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstAccess |= VK_ACCESS_TRANSFER_READ_BIT;
        }
        if (dst & BarrierStage_Indirect) {
            dstStages |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
            dstAccess |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        }
        if (dstStages == 0) {
            return;
        }
    }
    VkMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         clampStagesToQueue(dstStages, queueCaps()), 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
}

void VulkanCommandBuffer::readBarrier(u32 src) {
    if (cb_ == VK_NULL_HANDLE) {
        return;
    }
    // Execution-only WAR fence: prior reads at `src` finish before later
    // compute writes begin. No memory barrier — the reads consumed the
    // old data, there is nothing to make visible.
    VkPipelineStageFlags srcStages = 0;
    if (src & BarrierStage_Compute) {
        srcStages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    if (src & BarrierStage_Fragment) {
        srcStages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    if (src & BarrierStage_Vertex) {
        srcStages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    }
    if (src & BarrierStage_Transfer) {
        srcStages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (srcStages == 0) {
        return;
    }
    vkCmdPipelineBarrier(cb_, clampStagesToQueue(srcStages, queueCaps()),
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 0, nullptr);
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

bool VulkanCommandBuffer::recordHostUpdate(VkBuffer staging, u64 srcOffset,
                                           VkBuffer dst, u64 size,
                                           u64 dstOffset,
                                           VkPipelineStageFlags readStages,
                                           VkAccessFlags readAccess) {
    if (cb_ == VK_NULL_HANDLE || inPass_) {
        return false;
    }
    // readStages/readAccess are scoped to the buffer's declared usage
    // (bufferReadScope): the barrier only orders against the stages that
    // can actually read this buffer, leaving the rest of the frame free
    // to overlap the copy.

    // A pipeline barrier's execution dependency covers every command
    // submitted earlier on the queue — i.e. the previous frame's reads of
    // this buffer. WAR needs the execution ordering only, no source access.
    VkBufferMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = dst;
    barrier.offset = dstOffset;
    barrier.size = size;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags queueReadStages =
        clampStagesToQueue(readStages, queueCaps());
    vkCmdPipelineBarrier(cb_, queueReadStages, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);

    VkBufferCopy region {};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(cb_, staging, dst, 1, &region);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = readAccess;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_TRANSFER_BIT, queueReadStages,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    return true;
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
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, queueCaps());
    transitionLayout(cb_, t->image, t->aspect, 0, 1, t->arrayLayers, dstWas,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, queueCaps());

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
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queueCaps());
    transitionLayout(cb_, t->image, t->aspect, 0, 1, t->arrayLayers,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, queueCaps());
    s->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace rhi

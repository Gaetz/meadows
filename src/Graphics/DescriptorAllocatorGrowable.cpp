/**
 * @file DescriptorAllocatorGrowable.cpp
 * @brief Implementation of the dynamic Descriptor Set manager.
 *
 * This file contains the implementation of the DescriptorAllocatorGrowable class
 * which simplifies the management of Descriptor Pools and Sets in Vulkan.
 *
 */

#include "DescriptorAllocatorGrowable.h"

namespace graphics {

    // =========================================================================
    // Constructor
    // =========================================================================

    DescriptorAllocatorGrowable::DescriptorAllocatorGrowable(vk::Device device, u32 initialSets,  std::span<PoolSizeRatio> poolRatios) :
        device { device } {

        // Save ratios to create future pools with the same configuration
        ratios.clear();
        ratios.reserve(poolRatios.size());
        ratios = vector<PoolSizeRatio>(poolRatios.begin(), poolRatios.end());

        // Growth strategy: start with 1.5x the requested size
        // to have some headroom and reduce reallocations
        setsPerPool = static_cast<u32>(static_cast<float>(initialSets) * 1.5f);

        // Create the first pool, immediately ready for use
        const vk::DescriptorPool newPool = createPool(setsPerPool, ratios);
        readyPools.push_back(newPool);
    }

    // =========================================================================
    // Destructor
    // =========================================================================

    DescriptorAllocatorGrowable::~DescriptorAllocatorGrowable() {
        // IMPORTANT: In Vulkan, resources must be explicitly destroyed
        // Destroy all pools (ready and full)

        for (const auto p : readyPools) {
            device.destroyDescriptorPool(p, nullptr);
        }
        readyPools.clear();

        for (const auto p : fullPools) {
            device.destroyDescriptorPool(p, nullptr);
        }
        fullPools.clear();
    }

    // =========================================================================
    // Pool Reset
    // =========================================================================

    void DescriptorAllocatorGrowable::clear() {
        // resetDescriptorPool() frees all Descriptor Sets allocated from this pool
        // without destroying the pool itself. This is more efficient than recreating pools.

        // Reset already available pools
        for (const auto p : readyPools) {
            device.resetDescriptorPool(p, {});
        }

        // Full pools are reset and become available again
        for (const auto p : fullPools) {
            device.resetDescriptorPool(p, {});
            readyPools.push_back(p);  // The pool is usable again
        }
        fullPools.clear();

        // After clear(), all previously allocated Descriptor Sets are INVALID!
        // Do not use them anymore, they will point to recycled memory.
    }

    // =========================================================================
    // Descriptor Set Allocation
    // =========================================================================

    vk::DescriptorSet DescriptorAllocatorGrowable::allocate(const vk::DescriptorSetLayout layout, const void *pNext) {
        // Step 1: Get an available pool (or create a new one)
        vk::DescriptorPool poolToUse = getPool();

        // Step 2: Prepare allocation info
        // The layout describes the Descriptor Set structure (which bindings, which types)
        vk::DescriptorSetAllocateInfo allocInfo {};
        allocInfo.pNext = pNext;               // Optional Vulkan extensions
        allocInfo.descriptorPool = poolToUse;  // Pool to allocate from
        allocInfo.descriptorSetCount = 1;      // We allocate one set at a time
        allocInfo.pSetLayouts = &layout;       // Structure of the set to create

        // Step 3: Attempt allocation
        vk::DescriptorSet ds;
        const vk::Result result = device.allocateDescriptorSets(&allocInfo, &ds);

        // Step 4: Handle failure (pool full or fragmented)
        // This is where the "growable" magic happens!
        if (result == vk::Result::eErrorOutOfPoolMemory || result == vk::Result::eErrorFragmentedPool) {
            // The current pool is full, mark it as such
            fullPools.push_back(poolToUse);

            // Get a new pool (will be created if necessary)
            poolToUse = getPool();
            allocInfo.descriptorPool = poolToUse;

            // Retry with the new pool
            const vk::Result result2 = device.allocateDescriptorSets(&allocInfo, &ds);
            assert(result2 == vk::Result::eSuccess && "Descriptor set allocation failed after creating new pool");
        }

        // The used pool is put back in the available pools list
        // (it may be marked as full during a future allocation)
        readyPools.push_back(poolToUse);
        return ds;
    }

    // =========================================================================
    // Pool Management
    // =========================================================================

    vk::DescriptorPool DescriptorAllocatorGrowable::getPool() {
        vk::DescriptorPool newPool {};

        if (!readyPools.empty()) {
            // Simple case: we have an available pool, use it
            newPool = readyPools.back();
            readyPools.pop_back();
        } else {
            // No pool available: we need to create a new one
            newPool = createPool(setsPerPool, ratios);

            // Exponential growth strategy:
            // Each new pool is 50% larger than the previous one
            // This reduces the number of pools needed over time
            setsPerPool = static_cast<u32>(static_cast<float>(setsPerPool) * 1.5f);

            // Upper limit to avoid gigantic pools
            // 4092 sets per pool is a reasonable size
            if (setsPerPool > 4092) {
                setsPerPool = 4092;
            }
        }

        return newPool;
    }

    vk::DescriptorPool DescriptorAllocatorGrowable::createPool(u32 setCount, std::span<PoolSizeRatio> poolRatios) const {
        // Calculate sizes for each descriptor type
        // The ratio is multiplied by the number of sets to get the total descriptor count
        std::vector<vk::DescriptorPoolSize> poolSizes;
        for (const PoolSizeRatio ratio : poolRatios) {
            vk::DescriptorPoolSize descriptorPoolSize = {};
            descriptorPoolSize.type = ratio.type;
            // Example: if setCount=100 and ratio=2.0 for samplers,
            // the pool can contain 200 samplers in total
            descriptorPoolSize.descriptorCount = static_cast<u32>(ratio.ratio * static_cast<float>(setCount));
            poolSizes.push_back(descriptorPoolSize);
        }

        // Pool configuration
        vk::DescriptorPoolCreateInfo poolInfo = {};
        poolInfo.maxSets = setCount;  // Maximum number of Descriptor Sets in this pool
        poolInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();

        // Actual Vulkan pool creation
        return device.createDescriptorPool(poolInfo, nullptr);
    }

    // =========================================================================
    // Move Constructor and Assignment Operator (Move Semantics)
    // =========================================================================
    // These functions allow transferring ownership of Vulkan resources
    // from one allocator to another. This is essential because Vulkan
    // resources must only be destroyed once.

    DescriptorAllocatorGrowable::DescriptorAllocatorGrowable(DescriptorAllocatorGrowable&& other) noexcept
        : device(other.device), setsPerPool(other.setsPerPool), ratios(std::move(other.ratios)),
          fullPools(std::move(other.fullPools)), readyPools(std::move(other.readyPools)) {
        // The old object no longer owns the resources
        other.device = nullptr;
    }

    DescriptorAllocatorGrowable& DescriptorAllocatorGrowable::operator=(DescriptorAllocatorGrowable&& other) noexcept {
        if (this != &other) {
            // First, release our own resources before taking new ones
            for (const auto p : readyPools) {
                device.destroyDescriptorPool(p, nullptr);
            }
            for (const auto p: fullPools) {
                device.destroyDescriptorPool(p, nullptr);
            }

            // Transfer ownership
            device = other.device;
            setsPerPool = other.setsPerPool;
            ratios = std::move(other.ratios);
            fullPools = std::move(other.fullPools);
            readyPools = std::move(other.readyPools);

            // The old object no longer owns the resources
            other.device = nullptr;
        }
        return *this;
    }
}
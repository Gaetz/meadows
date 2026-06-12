#pragma once
#include "Types.h"

namespace graphics {

    /**
     * @class DescriptorAllocatorGrowable
     * @brief Dynamic Descriptor Set manager for Vulkan.
     *
     * ## What is a Descriptor Set?
     * In Vulkan, a Descriptor Set is a container that binds resources (textures, buffers,
     * samplers, etc.) to shaders. Shaders access these resources through descriptors.
     *
     * ## What is a Descriptor Pool?
     * A Descriptor Pool is a pre-allocated memory region from which Descriptor Sets are
     * allocated. It has a limited capacity and must be properly sized.
     *
     * ## Why this class?
     * Manual pool management is tedious: you need to predict the size, handle cases
     * where the pool is full, create new ones, etc.
     *
     * This class simplifies all of this by:
     * - Automatically creating new pools when existing ones are full
     * - Progressively increasing pool sizes (the "growable" strategy)
     * - Managing the pool lifecycle (creation, reset, destruction)
     *
     * @note This class uses move semantics and disallows copying to avoid
     *       Vulkan resource management issues.
     */
    class DescriptorAllocatorGrowable {
    public:

        /**
         * @struct PoolSizeRatio
         * @brief Defines the proportion of a descriptor type in the pool.
         *
         * For example, if you need 2 times more samplers than uniform buffers,
         * you can set a ratio of 2.0 for samplers and 1.0 for uniform buffers.
         */
        struct PoolSizeRatio {
            vk::DescriptorType type;  ///< Descriptor type (e.g., eUniformBuffer, eCombinedImageSampler, etc.)
            float ratio;               ///< Multiplier ratio for this type (will be multiplied by the number of sets)
        };

        DescriptorAllocatorGrowable() = default;

        /**
         * @brief Main constructor.
         * @param device The Vulkan logical device used to create pools.
         * @param initialSets Initial number of sets that the first pool can contain.
         * @param poolRatios Array defining descriptor types and their ratios.
         *
         * The first pool created will have a capacity of initialSets * 1.5 to provide headroom.
         */
        DescriptorAllocatorGrowable(vk::Device device, u32 initialSets, std::span<PoolSizeRatio> poolRatios);

        /**
         * @brief Destructor - releases all Vulkan pools.
         *
         * IMPORTANT: All allocated Descriptor Sets become invalid after destruction.
         */
        ~DescriptorAllocatorGrowable();

        /**
         * @brief Resets all pools without destroying them.
         *
         * After clear(), all previously allocated Descriptor Sets become invalid,
         * but pools can be reused for new allocations.
         * Useful between frames to recycle resources.
         */
        void clear();

        /**
         * @brief Allocates a new Descriptor Set.
         * @param layout The layout describing the Descriptor Set structure.
         * @param pNext Optional pointer for Vulkan extensions (pNext chain).
         * @return The newly allocated Descriptor Set.
         *
         * If the current pool is full, a new pool is automatically created.
         * This is the magic of the "growable" allocator!
         */
        vk::DescriptorSet allocate(vk::DescriptorSetLayout layout, const void* pNext = nullptr);

        // =====================================================================
        // Move Semantics
        // =====================================================================
        // These functions allow transferring resource ownership from one
        // allocator to another without copying data.

        DescriptorAllocatorGrowable(DescriptorAllocatorGrowable&& other) noexcept;
        DescriptorAllocatorGrowable& operator=(DescriptorAllocatorGrowable&& other) noexcept;

        // Copy is forbidden: each allocator must manage its own Vulkan resources
        DescriptorAllocatorGrowable(const DescriptorAllocatorGrowable&) = delete;
        DescriptorAllocatorGrowable& operator=(const DescriptorAllocatorGrowable&) = delete;

    private:
        /**
         * @brief Gets a ready-to-use pool or creates a new one.
         * @return An available pool for allocating Descriptor Sets.
         *
         * Uses the following strategy:
         * 1. If a pool is available in readyPools, use it
         * 2. Otherwise, create a new pool with 50% increased capacity
         */
        vk::DescriptorPool getPool();

        /**
         * @brief Creates a new Descriptor Pool.
         * @param setCount Maximum number of sets that the pool can contain.
         * @param poolRatios Ratios defining the capacity for each descriptor type.
         * @return The newly created Vulkan pool.
         */
        vk::DescriptorPool createPool(u32 setCount, std::span<PoolSizeRatio> poolRatios) const;

        // =====================================================================
        // Private Members
        // =====================================================================

        vk::Device device { nullptr };  ///< Vulkan logical device (needed to create/destroy pools)
        u32 setsPerPool { 0 };          ///< Capacity of the next pool to create (grows with each new pool)
        vector<PoolSizeRatio> ratios;   ///< Configuration of descriptor types and their proportions

        /**
         * @brief Completely filled pools.
         * These pools can no longer allocate new sets.
         * They will be reused after a call to clear().
         */
        vector<vk::DescriptorPool> fullPools;

        /**
         * @brief Pools available for new allocations.
         * After each allocation, the used pool is put back in this list
         * (unless it's full, in which case it goes to fullPools).
         */
        vector<vk::DescriptorPool> readyPools;

    };
}

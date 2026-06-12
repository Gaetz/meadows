/**
 * @file Buffer.h
 * @brief GPU buffer wrapper with automatic memory management via VMA.
 */

#pragma once

#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

namespace graphics {
    class VulkanContext;

    /**
     * @class Buffer
     * @brief Manages a Vulkan buffer with its memory allocation.
     *
     * ## What is a Vulkan Buffer?
     * A VkBuffer is a region of GPU-accessible memory used to store linear data.
     * Unlike images (which are organized as 2D/3D grids), buffers are simple arrays.
     *
     * ## Common buffer types:
     * - **Vertex Buffer**: Stores vertex data (positions, normals, UVs)
     * - **Index Buffer**: Stores triangle indices for indexed drawing
     * - **Uniform Buffer**: Stores shader constants (matrices, parameters)
     * - **Storage Buffer**: General-purpose read/write data for shaders
     * - **Staging Buffer**: Temporary CPU-visible buffer for uploading to GPU
     *
     * ## Memory types (VmaMemoryUsage):
     * - **VMA_MEMORY_USAGE_GPU_ONLY**: Fastest, but CPU can't access (use for static data)
     * - **VMA_MEMORY_USAGE_CPU_TO_GPU**: CPU can write, GPU can read (use for dynamic uniforms)
     * - **VMA_MEMORY_USAGE_GPU_TO_CPU**: GPU can write, CPU can read (use for readback)
     * - **VMA_MEMORY_USAGE_CPU_ONLY**: Staging buffers for upload
     *
     * ## Typical usage:
     * @code
     * // Create a vertex buffer on GPU
     * Buffer vertexBuffer(context, vertices.size() * sizeof(Vertex),
     *                     vk::BufferUsageFlagBits::eVertexBuffer,
     *                     VMA_MEMORY_USAGE_GPU_ONLY);
     *
     * // Create a uniform buffer (CPU-writable)
     * Buffer uniformBuffer(context, sizeof(UniformData),
     *                      vk::BufferUsageFlagBits::eUniformBuffer,
     *                      VMA_MEMORY_USAGE_CPU_TO_GPU);
     * uniformBuffer.write(&myData, sizeof(myData));
     * @endcode
     */
    class Buffer {
    public:
        Buffer() = default;

        /**
         * @brief Creates a buffer with the specified properties.
         * @param context The Vulkan context.
         * @param allocSize Size of the buffer in bytes.
         * @param usage How the buffer will be used (vertex, index, uniform, etc.).
         * @param memoryUsage Where to allocate the memory (GPU-only, CPU-visible, etc.).
         *
         * For CPU-visible memory types (CPU_ONLY, CPU_TO_GPU), the buffer is
         * automatically mapped and accessible via info.pMappedData.
         */
        Buffer(VulkanContext* context, size_t allocSize, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage);

        /**
         * @brief Destructor - automatically destroys the buffer and frees memory.
         */
        ~Buffer();

        // =====================================================================
        // Copy is disabled - each Buffer owns its GPU memory
        // =====================================================================
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

        // =====================================================================
        // Move is enabled - transfers ownership of GPU resources
        // =====================================================================
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        // =====================================================================
        // Memory Access
        // =====================================================================

        /**
         * @brief Maps the buffer memory for CPU access.
         * @param data Output pointer to the mapped memory.
         *
         * @note Only works for CPU-visible memory types.
         * @note For CPU_TO_GPU buffers, memory is already mapped (use info.pMappedData).
         */
        void map(void** data) const;

        /**
         * @brief Unmaps the buffer memory.
         */
        void unmap() const;

        /**
         * @brief Writes data to the buffer.
         * @param data Pointer to the source data.
         * @param size Number of bytes to write.
         * @param offset Offset into the buffer (default 0).
         *
         * This maps the memory, copies the data, then unmaps.
         * For frequently updated buffers, consider keeping the memory mapped.
         */
        void write(void* data, vk::DeviceSize size, vk::DeviceSize offset = 0);

        /**
         * @brief Destroys the buffer and frees its memory.
         *
         * Called automatically by the destructor, but can be called manually
         * to release resources early.
         */
        void destroy();

        /// Returns the Vulkan buffer handle
        vk::Buffer getBuffer() const { return buffer; }

    public:
        // =====================================================================
        // Buffer Resources (public for convenience)
        // =====================================================================

        VulkanContext* context {nullptr};  ///< Vulkan context reference
        vk::Buffer buffer {nullptr};        ///< The Vulkan buffer handle
        VmaAllocation allocation;           ///< VMA allocation handle
        VmaAllocationInfo info;             ///< Allocation info (includes pMappedData for CPU-visible buffers)
        vk::DeviceSize size {0};            ///< Size of the buffer in bytes
    };

    /**
     * @struct GPUMeshBuffers
     * @brief Holds the GPU buffers needed to render a mesh.
     *
     * A mesh needs:
     * - Index buffer: Which vertices form each triangle
     * - Vertex buffer: Vertex attributes (position, normal, UV, etc.)
     * - Vertex buffer address: For bindless rendering (buffer device address)
     *
     * ## What is Buffer Device Address?
     * Modern Vulkan allows shaders to access buffers directly via 64-bit addresses
     * instead of binding them as descriptors. This enables more flexible rendering
     * techniques like GPU-driven rendering.
     */
    struct GPUMeshBuffers {
        Buffer indexBuffer;                    ///< Buffer containing triangle indices
        Buffer vertexBuffer;                   ///< Buffer containing vertex data
        vk::DeviceAddress vertexBufferAddress; ///< GPU address for bindless access
    };
} // namespace graphics

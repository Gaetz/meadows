cmake_minimum_required(VERSION 3.20)
project(Meadows LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
# Function: Display download progress with ASCII bar
# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
function(download_with_progress lib_name)
    message(STATUS " ➤ ${lib_name} (downloading...)")
endfunction()

function(download_complete lib_name)
    message(STATUS " ✓ ${lib_name} (ready)")
endfunction()

# Dependencies
find_package(Vulkan REQUIRED)

# ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS " 📦 Downloading and configuring dependencies...")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# ──────────────────────────────────────────────────────────────────────
# SDL3
# ──────────────────────────────────────────────────────────────────────
set(SDL3_VERSION "3.4.0")
download_with_progress("SDL3 ${SDL3_VERSION}")
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL3_URL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-${SDL3_VERSION}.zip")
set(SDL3_ZIP "${CMAKE_BINARY_DIR}/_deps/SDL3-${SDL3_VERSION}.zip")
set(SDL3_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/SDL3-src")
if(NOT EXISTS ${SDL3_ZIP})
    file(DOWNLOAD ${SDL3_URL} ${SDL3_ZIP} SHOW_PROGRESS)
else()
    message(STATUS " ➤ SDL3 ${SDL3_VERSION} (cached)")
endif()
file(ARCHIVE_EXTRACT INPUT ${SDL3_ZIP} DESTINATION ${SDL3_SOURCE_DIR})
add_subdirectory(${SDL3_SOURCE_DIR}/SDL-release-${SDL3_VERSION} ${CMAKE_BINARY_DIR}/_deps/SDL3-build)
download_complete("SDL3 ${SDL3_VERSION}")

# ──────────────────────────────────────────────────────────────────────
# GLM
# ──────────────────────────────────────────────────────────────────────
download_with_progress("GLM")
set(GLM_URL "https://github.com/g-truc/glm/archive/refs/heads/master.zip")
set(GLM_ZIP "${CMAKE_BINARY_DIR}/_deps/GLM.zip")
set(GLM_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/GLM-src")
if(NOT EXISTS ${GLM_ZIP})
    file(DOWNLOAD ${GLM_URL} ${GLM_ZIP} SHOW_PROGRESS)
else()
    message(STATUS " ➤ GLM (cached)")
endif()
file(ARCHIVE_EXTRACT INPUT ${GLM_ZIP} DESTINATION ${GLM_SOURCE_DIR})
add_subdirectory(${GLM_SOURCE_DIR}/glm-master ${CMAKE_BINARY_DIR}/_deps/GLM-build)
download_complete("GLM")

# ──────────────────────────────────────────────────────────────────────
# ImGui
# ──────────────────────────────────────────────────────────────────────
download_with_progress("ImGui")
set(IMGUI_URL "https://github.com/ocornut/imgui/archive/refs/heads/docking.zip")
set(IMGUI_ZIP "${CMAKE_BINARY_DIR}/_deps/ImGui.zip")
set(IMGUI_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/ImGui-src")
if(NOT EXISTS ${IMGUI_ZIP})
    file(DOWNLOAD ${IMGUI_URL} ${IMGUI_ZIP} SHOW_PROGRESS)
else()
    message(STATUS " ➤ ImGui (cached)")
endif()
file(ARCHIVE_EXTRACT INPUT ${IMGUI_ZIP} DESTINATION ${IMGUI_SOURCE_DIR})
set(imgui_SOURCE_DIR ${IMGUI_SOURCE_DIR}/imgui-docking)
download_complete("ImGui")

# ──────────────────────────────────────────────────────────────────────
# VulkanMemoryAllocator
# ──────────────────────────────────────────────────────────────────────
download_with_progress("VulkanMemoryAllocator")
set(VMA_URL "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/heads/master.zip")
set(VMA_ZIP "${CMAKE_BINARY_DIR}/_deps/VMA.zip")
set(VMA_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/VMA-src")
if(NOT EXISTS ${VMA_ZIP})
    file(DOWNLOAD ${VMA_URL} ${VMA_ZIP} SHOW_PROGRESS)
else()
    message(STATUS " ➤ VulkanMemoryAllocator (cached)")
endif()
file(ARCHIVE_EXTRACT INPUT ${VMA_ZIP} DESTINATION ${VMA_SOURCE_DIR})
set(vulkanmemoryallocator_SOURCE_DIR ${VMA_SOURCE_DIR}/VulkanMemoryAllocator-master)
download_complete("VulkanMemoryAllocator")

# ──────────────────────────────────────────────────────────────────────
# VkBootstrap
# ──────────────────────────────────────────────────────────────────────
download_with_progress("VkBootstrap")
set(VK_BOOTSTRAP_DISABLE_WARNINGS ON CACHE BOOL "" FORCE)
# Use VkBootstrap version matching the Vulkan SDK version
if(Vulkan_VERSION)
    string(REGEX MATCH "([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ ${Vulkan_VERSION})
    set(VK_MAJOR ${CMAKE_MATCH_1})
    set(VK_MINOR ${CMAKE_MATCH_2})
    set(VK_PATCH ${CMAKE_MATCH_3})
    set(VKBOOTSTRAP_TAG "v${VK_MAJOR}.${VK_MINOR}.${VK_PATCH}")
else()
    # Fallback to known version
    set(VKBOOTSTRAP_TAG "v1.3.290")
endif()
set(VKBOOTSTRAP_URL "https://github.com/charles-lunarg/vk-bootstrap/archive/refs/tags/${VKBOOTSTRAP_TAG}.zip")
set(VKBOOTSTRAP_ZIP "${CMAKE_BINARY_DIR}/_deps/VkBootstrap.zip")
set(VKBOOTSTRAP_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/VkBootstrap-src")
if(NOT EXISTS ${VKBOOTSTRAP_ZIP})
    file(DOWNLOAD ${VKBOOTSTRAP_URL} ${VKBOOTSTRAP_ZIP} SHOW_PROGRESS)
else()
    message(STATUS " ➤ VkBootstrap (cached)")
endif()
file(ARCHIVE_EXTRACT INPUT ${VKBOOTSTRAP_ZIP} DESTINATION ${VKBOOTSTRAP_SOURCE_DIR})
if(Vulkan_VERSION AND VK_MAJOR)
    set(VKBOOTSTRAP_EXTRACTED "${VKBOOTSTRAP_SOURCE_DIR}/vk-bootstrap-${VK_MAJOR}.${VK_MINOR}.${VK_PATCH}")
else()
    set(VKBOOTSTRAP_EXTRACTED "${VKBOOTSTRAP_SOURCE_DIR}/vk-bootstrap-1.4.307")
endif()
add_subdirectory(${VKBOOTSTRAP_EXTRACTED} ${CMAKE_BINARY_DIR}/_deps/VkBootstrap-build)
download_complete("VkBootstrap")

# ──────────────────────────────────────────────────────────────────────
# fmt
# ──────────────────────────────────────────────────────────────────────
download_with_progress("fmt")
set(FMT_URL "https://github.com/fmtlib/fmt/archive/refs/heads/master.zip")
set(FMT_ZIP "${CMAKE_BINARY_DIR}/_deps/fmt.zip")
set(FMT_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/fmt-src")
if(NOT EXISTS ${FMT_ZIP})
    file(DOWNLOAD ${FMT_URL} ${FMT_ZIP} SHOW_PROGRESS)
else()
    message(STATUS " ➤ fmt (cached)")
endif()
file(ARCHIVE_EXTRACT INPUT ${FMT_ZIP} DESTINATION ${FMT_SOURCE_DIR})
add_subdirectory(${FMT_SOURCE_DIR}/fmt-master ${CMAKE_BINARY_DIR}/_deps/fmt-build)
download_complete("fmt")

# ──────────────────────────────────────────────────────────────────────
# stb_image.h
# ──────────────────────────────────────────────────────────────────────
download_with_progress("stb_image.h")
set(STB_IMAGE_HEADER "${CMAKE_BINARY_DIR}/_deps/stb/stb_image.h")
if(NOT EXISTS ${STB_IMAGE_HEADER})
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
        ${STB_IMAGE_HEADER}
        SHOW_PROGRESS
    )
else()
    message(STATUS " ➤ stb_image.h (cached)")
endif()
download_complete("stb_image.h")

# ──────────────────────────────────────────────────────────────────────
# fastgltf
# ──────────────────────────────────────────────────────────────────────
download_with_progress("fastgltf")
set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(FASTGLTF_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_URL "https://github.com/spnda/fastgltf/archive/refs/heads/main.zip")
set(FASTGLTF_ZIP "${CMAKE_BINARY_DIR}/_deps/fastgltf.zip")
set(FASTGLTF_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/fastgltf-src")
if(NOT EXISTS ${FASTGLTF_ZIP})
    file(DOWNLOAD ${FASTGLTF_URL} ${FASTGLTF_ZIP} SHOW_PROGRESS)
else()
    message(STATUS " ➤ fastgltf (cached)")
endif()
file(ARCHIVE_EXTRACT INPUT ${FASTGLTF_ZIP} DESTINATION ${FASTGLTF_SOURCE_DIR})
add_subdirectory(${FASTGLTF_SOURCE_DIR}/fastgltf-main ${CMAKE_BINARY_DIR}/_deps/fastgltf-build)
download_complete("fastgltf")

message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS " 📦 Downloading and configuring dependencies complete.")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")

# Shader compilation
find_program(GLSLC_EXECUTABLE glslc HINTS $ENV{VULKAN_SDK}/bin)
if(NOT GLSLC_EXECUTABLE)
    message(WARNING "glslc not found! Shaders will not be compiled.")
endif()

function(compile_shader SHADER_SOURCE SHADER_BINARY)
    add_custom_command(
        OUTPUT ${SHADER_BINARY}
        COMMAND ${GLSLC_EXECUTABLE} ${SHADER_SOURCE} -o ${SHADER_BINARY}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling ${SHADER_SOURCE} to ${SHADER_BINARY}"
    )
endfunction()

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SPV_DIR})

# Match the names expected by Renderer.cpp
compile_shader(${SHADER_DIR}/shader.vert ${SPV_DIR}/shader.vert.spv)
compile_shader(${SHADER_DIR}/shader.frag ${SPV_DIR}/shader.frag.spv)
compile_shader(${SHADER_DIR}/gradient.comp ${SPV_DIR}/gradient.comp.spv)
compile_shader(${SHADER_DIR}/gradientCustom.comp ${SPV_DIR}/gradientCustom.comp.spv)
compile_shader(${SHADER_DIR}/sky.comp ${SPV_DIR}/sky.comp.spv)
compile_shader(${SHADER_DIR}/coloredTriangle.vert ${SPV_DIR}/coloredTriangle.vert.spv)
compile_shader(${SHADER_DIR}/coloredTriangle.frag ${SPV_DIR}/coloredTriangle.frag.spv)
compile_shader(${SHADER_DIR}/coloredTriangleMesh.vert ${SPV_DIR}/coloredTriangleMesh.vert.spv)
compile_shader(${SHADER_DIR}/texImage.frag ${SPV_DIR}/texImage.frag.spv)
compile_shader(${SHADER_DIR}/mesh.vert ${SPV_DIR}/mesh.vert.spv)
compile_shader(${SHADER_DIR}/mesh.frag ${SPV_DIR}/mesh.frag.spv)
compile_shader(${SHADER_DIR}/g_buffer.vert ${SPV_DIR}/g_buffer.vert.spv)
compile_shader(${SHADER_DIR}/g_buffer.frag ${SPV_DIR}/g_buffer.frag.spv)
compile_shader(${SHADER_DIR}/deferred.vert ${SPV_DIR}/deferred.vert.spv)
compile_shader(${SHADER_DIR}/deferred.frag ${SPV_DIR}/deferred.frag.spv)

# Shadow mapping shaders
compile_shader(${SHADER_DIR}/shadowDepth.vert ${SPV_DIR}/shadowDepth.vert.spv)
compile_shader(${SHADER_DIR}/meshShadow.vert ${SPV_DIR}/meshShadow.vert.spv)
compile_shader(${SHADER_DIR}/meshShadow.frag ${SPV_DIR}/meshShadow.frag.spv)
compile_shader(${SHADER_DIR}/shadowDebug.vert ${SPV_DIR}/shadowDebug.vert.spv)
compile_shader(${SHADER_DIR}/shadowDebug.frag ${SPV_DIR}/shadowDebug.frag.spv)

add_custom_target(Shaders ALL
        DEPENDS
        ${SPV_DIR}/shader.vert.spv
        ${SPV_DIR}/shader.frag.spv
        ${SPV_DIR}/gradient.comp.spv
        ${SPV_DIR}/gradientCustom.comp.spv
        ${SPV_DIR}/sky.comp.spv
        ${SPV_DIR}/coloredTriangle.vert.spv
        ${SPV_DIR}/coloredTriangle.frag.spv
        ${SPV_DIR}/coloredTriangleMesh.vert.spv
        ${SPV_DIR}/texImage.frag.spv
        ${SPV_DIR}/mesh.vert.spv
        ${SPV_DIR}/mesh.frag.spv
        ${SPV_DIR}/g_buffer.vert.spv
        ${SPV_DIR}/g_buffer.frag.spv
        ${SPV_DIR}/deferred.vert.spv
        ${SPV_DIR}/deferred.frag.spv
        ${SPV_DIR}/shadowDepth.vert.spv
        ${SPV_DIR}/meshShadow.vert.spv
        ${SPV_DIR}/meshShadow.frag.spv
        ${SPV_DIR}/shadowDebug.vert.spv
        ${SPV_DIR}/shadowDebug.frag.spv
)

# Main Executable
add_executable(Meadows
    src/main.cpp
    src/Engine.cpp
    src/Engine.h
    src/BasicServices/Log.cpp
    src/BasicServices/Log.h
    src/BasicServices/File.cpp
    src/BasicServices/File.h
    src/BasicServices/FileWriter.cpp
    src/BasicServices/FileWriter.h
    src/BasicServices/Platform.h
    src/Graphics/VulkanContext.cpp
    src/Graphics/VulkanContext.h
    src/Graphics/Renderer.cpp
    src/Graphics/Renderer.h
    src/Graphics/MaterialPipeline.cpp
    src/Graphics/MaterialPipeline.h
    src/Graphics/Swapchain.cpp
    src/Graphics/Swapchain.h
    src/Graphics/Buffer.cpp
    src/Graphics/Buffer.h
    src/Graphics/VmaImplementation.cpp
    src/Graphics/VulkanInit.cpp
    src/Graphics/Types.h
    src/Graphics/Utils.cpp
    src/Graphics/DeletionQueue.cpp
    src/Graphics/DescriptorLayoutBuilder.cpp
    src/Graphics/DescriptorLayoutBuilder.hpp
        src/Graphics/DescriptorAllocator.cpp
        src/Graphics/DescriptorAllocator.h
        src/Graphics/PipelineCompute.cpp
        src/Graphics/PipelineCompute.h
        src/Graphics/ComputeEffect.cpp
        src/Graphics/ComputeEffect.h
        src/Graphics/PipelineBuilder.cpp
        src/Graphics/PipelineBuilder.h
        src/Graphics/VulkanLoader.cpp
        src/Graphics/VulkanLoader.h
        src/Graphics/LoadedGLTF.cpp
        src/Graphics/LoadedGLTF.h
        src/Graphics/DescriptorAllocatorGrowable.cpp
        src/Graphics/DescriptorAllocatorGrowable.h
        src/Graphics/DescriptorWriter.cpp
        src/Graphics/DescriptorWriter.h
        src/Graphics/Image.cpp
        src/Graphics/Image.h
        src/Graphics/Pipelines/GLTFMetallicRoughness.cpp
        src/Graphics/Pipelines/GLTFMetallicRoughness.h
        src/Graphics/Pipelines/ShadowPipeline.cpp
        src/Graphics/Pipelines/ShadowPipeline.h
        src/Graphics/Node.cpp
        src/Graphics/Node.h
        src/Graphics/Camera.cpp
        src/Graphics/Camera.h
        src/Graphics/ShadowMap.cpp
        src/Graphics/ShadowMap.h
        src/BasicServices/RenderingStats.h
        src/Graphics/RenderObject.h
        src/Scene.cpp
        src/Scene.h
        src/Graphics/Techniques/IRenderingTechnique.h
        src/Graphics/Techniques/BasicTechnique.cpp
        src/Graphics/Techniques/BasicTechnique.h
        src/Graphics/Techniques/ShadowMappingTechnique.cpp
        src/Graphics/Techniques/ShadowMappingTechnique.h
        src/Graphics/Techniques/DeferredRenderingTechnique.cpp
        src/Graphics/Techniques/DeferredRenderingTechnique.h
)

# Ensure shaders are compiled before building Meadows
add_dependencies(Meadows Shaders)

# Platform-specific sources
if(WIN32)
    target_sources(Meadows PRIVATE src/BasicServices/Platform_Win.cpp)
else()
    target_sources(Meadows PRIVATE src/BasicServices/Platform_Linux.cpp)
endif()

# ImGui Sources
target_sources(Meadows PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(Meadows PRIVATE 
    src 
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${vulkanmemoryallocator_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/_deps/stb
)

target_link_libraries(Meadows PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    glm::glm
    vk-bootstrap::vk-bootstrap
    fmt::fmt
    fastgltf::fastgltf
)

# Copy execution dependencies to output directory
add_custom_command(TARGET Meadows POST_BUILD
    # Copy shaders to output directory for execution
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${SPV_DIR} $<TARGET_FILE_DIR:Meadows>/shaders
    # Also copy SDL3 binary to output directory
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL3::SDL3>
    $<TARGET_FILE_DIR:Meadows>
    # Copy assets folder to output directory
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:Meadows>/assets
)

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
download_with_progress("SDL3")
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL3_URL "https://github.com/libsdl-org/SDL/archive/refs/heads/main.zip")
set(SDL3_ZIP "${CMAKE_BINARY_DIR}/_deps/SDL3.zip")
set(SDL3_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/SDL3-src")
if(NOT EXISTS ${SDL3_ZIP})
    file(DOWNLOAD ${SDL3_URL} ${SDL3_ZIP} SHOW_PROGRESS)
else()
    message(STATUS " ➤ SDL3 (cached)")
endif()
file(ARCHIVE_EXTRACT INPUT ${SDL3_ZIP} DESTINATION ${SDL3_SOURCE_DIR})
add_subdirectory(${SDL3_SOURCE_DIR}/SDL-main ${CMAKE_BINARY_DIR}/_deps/SDL3-build)
download_complete("SDL3")

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
add_subdirectory(${VKBOOTSTRAP_SOURCE_DIR}/vk-bootstrap-${VK_MAJOR}.${VK_MINOR}.${VK_PATCH} ${CMAKE_BINARY_DIR}/_deps/VkBootstrap-build)
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
    src/Graphics/Pipeline.cpp
    src/Graphics/Pipeline.h
    src/Graphics/Swapchain.cpp
    src/Graphics/Swapchain.h
    src/Graphics/Buffer.cpp
    src/Graphics/Buffer.h
    src/Graphics/VulkanInit.cpp
    src/Graphics/Types.h
    src/Graphics/Utils.cpp
    src/Graphics/DeletionQueue.cpp
    ${SPV_DIR}/shader.vert.spv
    ${SPV_DIR}/shader.frag.spv
)

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
)

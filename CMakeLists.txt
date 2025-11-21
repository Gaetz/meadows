cmake_minimum_required(VERSION 3.20)
project(Meadows LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies
find_package(Vulkan REQUIRED)

include(FetchContent)

# SDL3
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG main
)
FetchContent_MakeAvailable(SDL3)

# GLM
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG master  # Use master for latest CMake compatibility
)
FetchContent_MakeAvailable(glm)

# ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# VulkanMemoryAllocator
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG master
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

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
    src/Graphics/Types.h
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
)

target_link_libraries(Meadows PRIVATE
    SDL3::SDL3
    Vulkan::Vulkan
    glm::glm
)

# Copy shaders to output directory for execution
add_custom_command(TARGET Meadows POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${SPV_DIR} $<TARGET_FILE_DIR:Meadows>/shaders
)

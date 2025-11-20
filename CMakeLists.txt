cmake_minimum_required(VERSION 3.25)

project(VulkanEngine LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Dependencies
include(FetchContent)

# SDL3
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG preview-3.1.3 # Using a recent preview tag or main
)
FetchContent_MakeAvailable(SDL3)

# GLM
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)
FetchContent_MakeAvailable(glm)

# Vulkan
find_package(Vulkan REQUIRED)

# Vulkan Memory Allocator
FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.1.0
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.1
)
FetchContent_MakeAvailable(imgui)

# ShaderCompiler (glslang) - Optional but good for runtime compilation if needed, 
# but for now we might just rely on precompiled spir-v or system tools. 
# Let's stick to basic setup first.

# Sources
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

# ImGui Sources (Need to include backends)
set(IMGUI_DIR ${imgui_SOURCE_DIR})
list(APPEND SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    src/backends/imgui_impl_sdl3.cpp
    src/backends/imgui_impl_vulkan.cpp
)

add_executable(VulkanEngine ${SOURCES})

# Includes
target_include_directories(VulkanEngine PRIVATE 
    src 
    src/backends
    ${IMGUI_DIR} 
    ${IMGUI_DIR}/backends
    ${vulkanmemoryallocator_SOURCE_DIR}/include
)

# Link Libraries
target_link_libraries(VulkanEngine PRIVATE 
    SDL3::SDL3 
    glm::glm 
    Vulkan::Vulkan
)

# Platform specific flags
if(WIN32)
    # Ensure we are using the correct subsystem for windows (no console window if desired, but good for debug)
    # add_compile_definitions(VK_USE_PLATFORM_WIN32_KHR) # SDL handles this usually
elseif(UNIX AND NOT APPLE)
    # Linux specific
    # add_compile_definitions(VK_USE_PLATFORM_XCB_KHR) # or Wayland
endif()

# Shader Compilation
find_program(GLSLC_EXECUTABLE glslc HINTS 
    $ENV{VULKAN_SDK}/Bin
    "C:/VulkanSDK/1.4.328.1/Bin"
)

if(GLSLC_EXECUTABLE)
    message(STATUS "Found glslc: ${GLSLC_EXECUTABLE}")
    
    set(SHADERS 
        shaders/shader.vert
        shaders/shader.frag
    )
    
    foreach(SHADER ${SHADERS})
        get_filename_component(FILENAME ${SHADER} NAME)
        set(SPIRV "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${FILENAME}.spv")
        
        add_custom_command(
            OUTPUT ${SPIRV}
            COMMAND ${GLSLC_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} -o ${SPIRV}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}
            COMMENT "Compiling ${FILENAME} to SPIR-V"
        )
        
        list(APPEND SPIRV_SHADERS ${SPIRV})

        add_custom_command(TARGET VulkanEngine POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:VulkanEngine>/shaders
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPIRV} $<TARGET_FILE_DIR:VulkanEngine>/shaders/
        )
    endforeach()
    
    add_custom_target(Shaders ALL DEPENDS ${SPIRV_SHADERS})
    add_dependencies(VulkanEngine Shaders)
else()
    message(WARNING "glslc not found! Shaders will not be compiled.")
endif()

# Copy shaders to bin (source shaders, optional if we only use spv)
# file(COPY ${CMAKE_SOURCE_DIR}/shaders DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

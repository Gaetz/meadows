cmake_minimum_required(VERSION 3.25)
# C is needed for vendored C sources (glad).
project(true-adventurer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies are pinned here and fetched at configure time, identically on
# every OS (§3.1). Set the CPM_SOURCE_CACHE env var to share downloads between
# build directories.
include(cmake/CPM.cmake)

CPMAddPackage(
    NAME SDL3
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE
    OPTIONS
        "SDL_SHARED OFF"
        "SDL_STATIC ON"
        "SDL_TEST_LIBRARY OFF"
        "SDL_EXAMPLES OFF"
)

CPMAddPackage("gh:gabime/spdlog@1.17.0")
CPMAddPackage("gh:g-truc/glm#1.0.1")
CPMAddPackage("gh:doctest/doctest@2.5.2")
CPMAddPackage("gh:marzer/tomlplusplus@3.4.0")

# flecs — the ECS (Phase 2). Pinned like every dep (§3.1); flecs breaks API
# across majors, so version bumps are deliberate. Static lib only (no DLL on
# Windows); confined to the meadows-ecs target — meadows-data stays flecs-free.
CPMAddPackage(
    NAME flecs
    GITHUB_REPOSITORY SanderMertens/flecs
    GIT_TAG v4.1.5
    GIT_SHALLOW TRUE
    OPTIONS
        "FLECS_SHARED OFF"
        "FLECS_STATIC ON"
        "FLECS_TESTS OFF"
)

# flecs exports its include dir as a normal (non-system) PUBLIC include, which
# leaks its own /W4 -Wextra warnings into our TUs. Mark it system for every
# consumer so only our code is held to our warning bar.
if(TARGET flecs_static)
    get_target_property(_flecs_inc flecs_static INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(flecs_static PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_flecs_inc}")
endif()

# Lua (Phase 4 scripting). lua/lua ships no CMake; build a static lib from its
# sources (same pattern as imgui/stb below). Exclude the interpreter/compiler
# mains (lua.c, luac.c) and the single-file build (onelua.c).
CPMAddPackage(
    NAME lua
    GITHUB_REPOSITORY lua/lua
    GIT_TAG v5.4.8
    DOWNLOAD_ONLY TRUE
)
file(GLOB lua_sources ${lua_SOURCE_DIR}/*.c)
list(REMOVE_ITEM lua_sources
    ${lua_SOURCE_DIR}/lua.c ${lua_SOURCE_DIR}/luac.c ${lua_SOURCE_DIR}/onelua.c)
add_library(lua STATIC ${lua_sources})
target_include_directories(lua SYSTEM PUBLIC ${lua_SOURCE_DIR})

# sol2 — header-only C++ binding over Lua. Confined to the meadows-script target.
CPMAddPackage(
    NAME sol2
    GITHUB_REPOSITORY ThePhD/sol2
    GIT_TAG v3.5.0
    DOWNLOAD_ONLY TRUE
)
add_library(sol2 INTERFACE)
target_include_directories(sol2 SYSTEM INTERFACE ${sol2_SOURCE_DIR}/include)
target_link_libraries(sol2 INTERFACE lua)
target_compile_definitions(sol2 INTERFACE SOL_ALL_SAFETIES_ON=1)

# stb has no tags: pinned by commit. Header-only; the implementation TU
# lives in engine/assets/StbImpl.cpp.
CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG 31c1ad374564
    DOWNLOAD_ONLY TRUE
)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# Dear ImGui ships no CMakeLists; build it as a small static lib with the
# SDL3 + OpenGL3 backends (the GL3 backend embeds its own function loader).
CPMAddPackage(
    NAME imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG v1.92.8
    DOWNLOAD_ONLY TRUE
)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PRIVATE SDL3::SDL3)

function(meadows_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

add_subdirectory(extern/glad)
add_subdirectory(engine)
add_subdirectory(data)
add_subdirectory(gameplay)
add_subdirectory(script)
add_subdirectory(world)
add_subdirectory(game)
add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)

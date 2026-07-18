# 3.26: copy_directory_if_different (the per-build game-data sync).
cmake_minimum_required(VERSION 3.26)
# C is needed for vendored C sources (glad).
project(true-adventurer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies are pinned here and fetched at configure time, identically on
# every OS (§3.1). Set the CPM_SOURCE_CACHE env var to share downloads between
# build directories.
# Every dependency below is pinned to an exact tag or commit, so re-querying
# each remote on every configure buys nothing — and it cost ~5 min 40 s per
# no-op configure (8 `git fetch` round-trips), besides failing outright when
# the network hiccups. Skipping the update stage takes that to seconds; the
# download stage still runs for anything not yet populated.
# NOTE: after CHANGING a pin, if a dependency does not re-populate, delete
# cmake-build-*/\_deps/<name>-* to force it.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Skip git update for already-populated pinned dependencies")

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

# Jolt (§3 physics, horizontal pass H3). CMake lives in Build/. Options trim
# every extra target; USE_STATIC_MSVC_RUNTIME_LIBRARY must be OFF to match our
# dynamic CRT. CROSS_PLATFORM_DETERMINISTIC keeps the §8 determinism door open.
CPMAddPackage(
    NAME JoltPhysics
    GITHUB_REPOSITORY jrouwe/JoltPhysics
    GIT_TAG v5.2.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build
    OPTIONS
        "TARGET_UNIT_TESTS OFF"
        "TARGET_HELLO_WORLD OFF"
        "TARGET_PERFORMANCE_TEST OFF"
        "TARGET_SAMPLES OFF"
        "TARGET_VIEWER OFF"
        "USE_STATIC_MSVC_RUNTIME_LIBRARY OFF"
        "CROSS_PLATFORM_DETERMINISTIC ON"
        "INTERPROCEDURAL_OPTIMIZATION OFF"
        "ENABLE_ALL_WARNINGS OFF"
)
if(TARGET Jolt)
    get_target_property(_jolt_inc Jolt INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(Jolt PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_jolt_inc}")
endif()

# miniaudio (§3 audio, H6) — single header; implementation TU in
# engine/audio/Audio.cpp.
CPMAddPackage(
    NAME miniaudio
    GITHUB_REPOSITORY mackron/miniaudio
    GIT_TAG 0.11.21
    GIT_SHALLOW TRUE
    DOWNLOAD_ONLY TRUE
)
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})

# FreeType + RmlUi (§3 game UI, H4). FreeType is trimmed of every optional
# codec; RmlUi 6 picks up the existing Freetype::Freetype target instead of
# find_package when it is already defined.
CPMAddPackage(
    NAME freetype
    GITHUB_REPOSITORY freetype/freetype
    GIT_TAG VER-2-13-3
    GIT_SHALLOW TRUE
    OPTIONS
        "FT_DISABLE_ZLIB ON"
        "FT_DISABLE_BZIP2 ON"
        "FT_DISABLE_PNG ON"
        "FT_DISABLE_HARFBUZZ ON"
        "FT_DISABLE_BROTLI ON"
)
if(TARGET freetype AND NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
CPMAddPackage(
    NAME RmlUi
    GITHUB_REPOSITORY mikke89/RmlUi
    GIT_TAG 6.1
    GIT_SHALLOW TRUE
    OPTIONS
        "BUILD_SHARED_LIBS OFF"
        "RMLUI_SAMPLES OFF"
        "RMLUI_LUA_BINDINGS OFF"
)

# cgltf — header-only glTF 2.0 loader (§3 mesh/model). The implementation TU
# lives in engine/assets/GltfMesh.cpp.
CPMAddPackage(
    NAME cgltf
    GITHUB_REPOSITORY jkuhlmann/cgltf
    GIT_TAG v1.15
    DOWNLOAD_ONLY TRUE
)
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})

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
    # The -docking flavour of the same release (chantier 8.7b): the DB
    # editor is a dockspace. Same API surface otherwise; ImGuizmo and the
    # vendored node-editor compile against it unchanged.
    GIT_TAG v1.92.8-docking
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

# ImGuizmo (chantier 2 B3): translate/rotate/scale gizmos for the level
# editor. MIT, plain sources over the imgui headers.
CPMAddPackage(
    NAME imguizmo
    GITHUB_REPOSITORY CedricGuillemet/ImGuizmo
    # No release tag tracks recent imgui; pin the master commit compatible
    # with imgui 1.92 (2026-07 state).
    GIT_TAG 87fb88b13a4f6bb04cd6ecd4e9d1625929935ff1
    DOWNLOAD_ONLY TRUE
)
add_library(imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(imguizmo SYSTEM PUBLIC
    ${imguizmo_SOURCE_DIR}/src)
target_compile_definitions(imguizmo PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imguizmo PUBLIC imgui)

function(meadows_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# --- RHI backend gating (§2.1) --------------------------------------------
# Backend *selection* is runtime (a preference/fallback chain, Vulkan->GL);
# WHICH backends are compiled into the binary is this compile-time gate. The
# GL backends are always built for now (the demo and the 2D/dev-UI path run on
# them); Vulkan is the intended FINAL renderer, brought up brick by brick
# (docs/VULKAN.md). Defaults: ON on macOS (Apple GL 4.1 cannot run the 3D
# renderer, so Vulkan is the only path there); opt-in elsewhere until it is
# functional. macOS ships MoltenVK; Linux/Windows use their native loader.
if(APPLE)
    option(MEADOWS_RHI_VULKAN "Build the Vulkan RHI backend (+ MoltenVK)" ON)
else()
    option(MEADOWS_RHI_VULKAN "Build the Vulkan RHI backend" OFF)
endif()

if(MEADOWS_RHI_VULKAN)
    # Locates the headers + loader (Vulkan::Vulkan). On this dev Mac the LunarG
    # SDK installs under /usr/local; hint it in case VULKAN_SDK is unset.
    find_package(Vulkan REQUIRED)
    message(STATUS "Meadows: Vulkan RHI backend ON — ${Vulkan_LIBRARY}")
    # The loader's install name is @rpath/libvulkan.1.dylib, so every executable
    # linking it needs an RPATH to the SDK's lib dir or it fails to start
    # ("no LC_RPATH's found"). Set before any target is defined so they all
    # inherit it.
    get_filename_component(MEADOWS_VULKAN_LIBDIR "${Vulkan_LIBRARY}" DIRECTORY)
    list(APPEND CMAKE_BUILD_RPATH "${MEADOWS_VULKAN_LIBDIR}")

    # shaderc — GLSL -> SPIR-V at runtime (V3), so ShaderLibrary's hot reload
    # keeps working on Vulkan (a build-time .spv step would kill it). Ships
    # with the Vulkan SDK; the shared lib is linked rather than
    # shaderc_combined.a, which is a 1.3 GB archive.
    find_library(MEADOWS_SHADERC_LIBRARY NAMES shaderc_shared shaderc
                 HINTS "${MEADOWS_VULKAN_LIBDIR}")
    if(NOT MEADOWS_SHADERC_LIBRARY)
        message(FATAL_ERROR
            "Meadows: shaderc not found next to the Vulkan loader "
            "(${MEADOWS_VULKAN_LIBDIR}). It ships with the LunarG SDK.")
    endif()
    message(STATUS "Meadows: shaderc — ${MEADOWS_SHADERC_LIBRARY}")

    # VMA — GPU memory sub-allocation (V2). Header-only, so DOWNLOAD_ONLY keeps
    # it out of the build graph: one TU defines VMA_IMPLEMENTATION.
    CPMAddPackage(
        NAME VulkanMemoryAllocator
        GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
        GIT_TAG v3.1.0
        GIT_SHALLOW TRUE
        DOWNLOAD_ONLY TRUE
    )
endif()

add_subdirectory(extern/glad)
# imgui-node-editor (chantier 8.6): the node canvas behind the graph
# editors — VENDORED with two imgui-1.92 fixes, see its README.md. Its
# internal json persistence is disabled at runtime (SettingsFile =
# nullptr) — node positions live in the editor side-store.
add_subdirectory(extern/imgui-node-editor)
add_subdirectory(engine)
add_subdirectory(data)
add_subdirectory(gameplay)
add_subdirectory(script)
add_subdirectory(quest)
add_subdirectory(world)
add_subdirectory(game)
add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)

# meadows — the engine BASE library: core, reflection, platform, assets, anim,
# fx. The game links against this; nothing in here may depend on game code.
# The renderer/RHI live in meadows-render below (audit U9-3): the sim libs
# (data/ecs/gameplay/world/script/narrative) link only this base, and the
# meadows-simlink target (tests/) proves at LINK level that no sim object
# references a render/rhi/GL symbol (§2.10). Engine.cpp/Game.cpp stay here
# (they reference render symbols, resolved when the final exe links
# meadows-render — the sim never pulls their objects).
add_library(meadows STATIC
    Engine.cpp
    Game.cpp
    anim/Anim.cpp
    assets/AssetDatabase.cpp
    fx/Particles.cpp
    assets/GltfMesh.cpp
    assets/Image.cpp
    assets/StbImpl.cpp
    core/Guid.cpp
    core/Jobs.cpp
    core/Log.cpp
    reflect/Registry.cpp
    platform/common/Input.cpp
    platform/common/Paths.cpp
    platform/common/Window.cpp
)

# Per-OS platform sources are selected here at configure time (§3.1), e.g.:
#   if(WIN32)
#       target_sources(meadows PRIVATE platform/win32/Paths.cpp)
#   elseif(LINUX)
#       target_sources(meadows PRIVATE platform/posix/Paths.cpp)
#   endif()
# Nothing OS-specific is needed yet: SDL3 covers it all so far.

target_include_directories(meadows PUBLIC ${PROJECT_SOURCE_DIR})

# imgui is PUBLIC: the game emits its own dev-UI widgets in Game::drawUi.
# glad moved to meadows-render with the GL backend (audit U9-3).
target_link_libraries(meadows
    PUBLIC spdlog::spdlog glm::glm imgui
    PRIVATE SDL3::SDL3 stb cgltf
)

# Compile LOG_TRACE/LOG_DEBUG out of release builds.
target_compile_definitions(meadows PUBLIC
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

meadows_set_warnings(meadows)

# meadows-render — the renderer + RHI, split out of the base lib (audit U9-3)
# so the sim can be link-proven render-free (§2.10). Same sources, same flags:
# behavior is identical by construction; only archive membership changed.
# GlContext lives here (the GL half of the platform layer), ImGuiLayer too
# (the assumed dev-UI GL exception, §3).
add_library(meadows-render STATIC
    platform/common/GlContext.cpp
    render/FlyCamera.cpp
    render/MeshBuilder.cpp
    render/ShaderLibrary.cpp
    render/SpriteRenderer.cpp
    render/landscape/ChunkOcclusion.cpp
    render/landscape/GpuOcclusion.cpp
    render/landscape/GrassSystem.cpp
    render/landscape/PostFx.cpp
    render/landscape/ShadowMapper.cpp
    render/landscape/SkySystem.cpp
    render/landscape/SplatTextures.cpp
    render/landscape/TerrainLightMap.cpp
    render/landscape/TerrainNoise.cpp
    render/landscape/TerrainSystem.cpp
    render/landscape/TreeGenerator.cpp
    render/landscape/VegetationSystem.cpp
    render/landscape/WaterSystem.cpp
    rhi/Device.cpp
    rhi/backends/gl/GlDevice.cpp
    rhi/backends/gl/GlDeviceBase.cpp
    rhi/backends/gl/GlDevice46.cpp
    rhi/backends/gl/GlDevice41.cpp
    ui/ImGuiLayer.cpp
)
target_link_libraries(meadows-render
    PUBLIC meadows
    PRIVATE SDL3::SDL3 glad stb
)

# Debug builds hot-reload shaders straight from the repo's source directory
# (editing a .vert/.frag reloads live); other builds read <exe>/data/shaders.
target_compile_definitions(meadows-render PRIVATE
    $<$<CONFIG:Debug>:MEADOWS_SHADER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/render/landscape/shaders">
)

meadows_set_warnings(meadows-render)

# meadows-physics — the Jolt seam (horizontal pass H3). A separate lib so
# Jolt stays PRIVATE: consumers see only the phys:: facade (engine/physics/
# Physics.hpp, pimpl — §3.1). Headless: the sim side owns physics.
add_library(meadows-physics STATIC
    physics/Physics.cpp
)
target_link_libraries(meadows-physics
    PUBLIC meadows
    PRIVATE Jolt
)
meadows_set_warnings(meadows-physics)

# meadows-ui — the RmlUi seam (horizontal pass H4). RmlUi/FreeType stay
# PRIVATE; consumers see only ui::UiSystem (engine/ui/UiSystem.hpp).
add_library(meadows-ui STATIC
    ui/UiSystem.cpp
)
target_link_libraries(meadows-ui
    PUBLIC meadows-render # UiSystem renders through the RHI
    PRIVATE RmlUi::RmlUi
)
meadows_set_warnings(meadows-ui)

# meadows-audio — the miniaudio seam (horizontal pass H6). miniaudio stays
# PRIVATE; consumers see only audio::AudioSystem.
add_library(meadows-audio STATIC
    audio/Audio.cpp
)
target_link_libraries(meadows-audio
    PUBLIC meadows
    PRIVATE miniaudio
)
meadows_set_warnings(meadows-audio)

# meadows-ecs — the ECS layer (flecs). Kept a separate target so flecs stays
# confined to the runtime: meadows (base engine) and meadows-data both remain
# flecs-free. Components are plain reflected structs; flecs only appears in
# ecs/world/system TUs, never in component-data headers.
add_library(meadows-ecs STATIC
    ecs/World.cpp
)
target_link_libraries(meadows-ecs PUBLIC meadows flecs::flecs_static)

# flecs' component-registration template trips C4702 (unreachable code) under
# MSVC /W4; the SYSTEM include doesn't cover warnings raised while instantiating
# a template in our TUs. It is benign and internal to flecs — suppress it for
# every flecs consumer (PUBLIC), not in our own warning baseline.
target_compile_options(meadows-ecs PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/wd4702>)

meadows_set_warnings(meadows-ecs)

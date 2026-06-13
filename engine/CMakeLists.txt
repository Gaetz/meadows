# meadows — the engine library. The game links against this; nothing in here
# may depend on game code.
add_library(meadows STATIC
    Engine.cpp
    assets/AssetDatabase.cpp
    assets/Image.cpp
    assets/StbImpl.cpp
    core/Guid.cpp
    core/Jobs.cpp
    core/Log.cpp
    reflect/Registry.cpp
    platform/common/GlContext.cpp
    platform/common/Input.cpp
    platform/common/Paths.cpp
    platform/common/Window.cpp
    render/SpriteRenderer.cpp
    rhi/Device.cpp
    rhi/backends/gl/GlDevice.cpp
    ui/ImGuiLayer.cpp
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
target_link_libraries(meadows
    PUBLIC spdlog::spdlog glm::glm imgui
    PRIVATE SDL3::SDL3 glad stb
)

# Compile LOG_TRACE/LOG_DEBUG out of release builds.
target_compile_definitions(meadows PUBLIC
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<NOT:$<CONFIG:Debug>>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)

meadows_set_warnings(meadows)

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

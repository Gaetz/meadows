# The cooker shares game/AllForms.cpp — the SINGLE registerAllFormTypes()
# aggregator — so the cooker and the game exe can never drift (a past drift
# left the cooker registering CoreForms only, silently dropping records).
# The file is headless (data/world/gameplay/quest only); world + narrative
# pull every form family into the link/include closure.
# meadows-render: the `terrain-pad` subcommand bakes heights through
# render::terrain::height — a TOOL may use the renderer's CPU functions;
# the sim libs themselves stay render-free (see tests/meadows-simlink).
# WeaponMeshes: the validate subcommand whitelists the runtime-injected
# procedural mesh guids (headless mesh builders — CPU MeshData only).
add_executable(cooker cooker/Main.cpp cooker/TextureCook.cpp
               cooker/TerrainMap.cpp cooker/ErosionBench.cpp
               cooker/WaterReplayTool.cpp
    cooker/WaterSolveTool.cpp
               ${PROJECT_SOURCE_DIR}/game/AllForms.cpp
               ${PROJECT_SOURCE_DIR}/game/WeaponMeshes.cpp)
# stb/bc7enc/toml++: the cook-terrain-materials subcommand decodes, resizes
# and BC-encodes textures offline — the runtime never links the encoders.
target_link_libraries(cooker PRIVATE meadows-data meadows-world
                      meadows-narrative meadows-render
                      stb bc7enc tomlplusplus::tomlplusplus)
target_compile_definitions(cooker PRIVATE TOML_EXCEPTIONS=0)
meadows_set_warnings(cooker)

# vksmoke — Vulkan bring-up harness (chantier VULKAN, V1). Drives the backend's
# presentation path without the Engine loop, which is still tied to the GL
# SpriteRenderer/ImGui until V3/V6. Built only with the Vulkan backend.
if(MEADOWS_RHI_VULKAN)
    add_executable(vksmoke vksmoke/Main.cpp)
    # meadows-ui too: the harness checks the in-game UI renders on Vulkan
    # (RmlUi already goes through the RHI).
    target_link_libraries(vksmoke PRIVATE meadows-render meadows-ui)
    meadows_set_warnings(vksmoke)
endif()

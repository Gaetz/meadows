# imgui-node-editor (thedmd), VENDORED — the node canvas of the graph
# editors (chantier 8.6). Upstream develop @ b302971 plus the two Dear
# ImGui 1.92 fixes recorded in upstream-imgui192.patch (unguarded
# float*ImVec2 operator, removed ImRect::Floor) — see README.md.
# Vendored (not CPM+PATCHES) because a patch step re-runs over the shared
# CPM_SOURCE_CACHE source on every new build dir and fails as "already
# applied" — committed sources have no populate step at all.
add_library(imgui-node-editor STATIC
    crude_json.cpp
    imgui_canvas.cpp
    imgui_node_editor.cpp
    imgui_node_editor_api.cpp
)
target_include_directories(imgui-node-editor SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(imgui-node-editor PRIVATE
    IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui-node-editor PUBLIC imgui)

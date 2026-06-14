# meadows-script — the Lua scripting layer (Phase 4): the sol2 VM, ScriptVars,
# the `self` proxy, and (later) the event bus + coroutine scheduler. sol2/Lua
# are PRIVATE — confined to this lib's .cpp, never leaking into engine headers.
add_library(meadows-script STATIC
    Vm.cpp
    ScriptVars.cpp
)

target_link_libraries(meadows-script
    PUBLIC meadows-gameplay
    PRIVATE sol2
)

meadows_set_warnings(meadows-script)

# meadows-data — the data model: forms, plugins, field-level resolution.
# Depends on the engine core/reflect layers only; rendering must never leak
# in here (the build enforces what §4 demands).
add_library(meadows-data STATIC
    forms/CoreForms.cpp
    forms/FormDatabase.cpp
    plugins/PluginLoader.cpp
    plugins/Resolver.cpp
)

target_link_libraries(meadows-data
    PUBLIC meadows
    PRIVATE tomlplusplus::tomlplusplus
)

# parse_result API instead of exceptions (§8: recoverable errors as values).
target_compile_definitions(meadows-data PRIVATE TOML_EXCEPTIONS=0)

meadows_set_warnings(meadows-data)

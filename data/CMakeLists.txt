# meadows-data — the data model: forms, plugins, field-level resolution.
# Depends on the engine core/reflect layers only; rendering must never leak
# in here (the build enforces what §4 demands).
add_library(meadows-data STATIC
    forms/AnimForms.cpp
    forms/AudioForms.cpp
    forms/CoreForms.cpp
    forms/FormDatabase.cpp
    forms/LandscapeForms.cpp
    forms/LocForms.cpp
    forms/UiForms.cpp
    forms/VisualForms.cpp
    plugins/BinaryFormat.cpp
    plugins/EditSession.cpp
    plugins/PluginConfig.cpp
    plugins/PluginLoader.cpp
    plugins/Resolver.cpp
    plugins/Synthesis.cpp
    plugins/TomlWriter.cpp
)

target_link_libraries(meadows-data
    PUBLIC meadows
    PRIVATE tomlplusplus::tomlplusplus
)

# parse_result API instead of exceptions (§8: recoverable errors as values).
target_compile_definitions(meadows-data PRIVATE TOML_EXCEPTIONS=0)

meadows_set_warnings(meadows-data)

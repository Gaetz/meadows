add_executable(meadows-tests
    Main.cpp
    AbilitySystemTest.cpp
    AssetsTest.cpp
    CellLoaderTest.cpp
    CookerTest.cpp
    EcsTest.cpp
    FormsTest.cpp
    GameplayEffectsTest.cpp
    GameplayTagsTest.cpp
    GuidTest.cpp
    JobsTest.cpp
    PluginLoaderTest.cpp
    ReflectTest.cpp
    ResolverTest.cpp
    SceneSubmitTest.cpp
    SpawnerTest.cpp
    WorldModelTest.cpp
)
target_link_libraries(meadows-tests PRIVATE meadows meadows-data meadows-ecs
                      meadows-world meadows-runtime meadows-gameplay
                      doctest::doctest)
meadows_set_warnings(meadows-tests)
add_test(NAME meadows COMMAND meadows-tests)

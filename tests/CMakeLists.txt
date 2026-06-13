add_executable(meadows-tests
    Main.cpp
    AssetsTest.cpp
    CookerTest.cpp
    EcsTest.cpp
    FormsTest.cpp
    GuidTest.cpp
    JobsTest.cpp
    PluginLoaderTest.cpp
    ReflectTest.cpp
    ResolverTest.cpp
    WorldModelTest.cpp
)
target_link_libraries(meadows-tests PRIVATE meadows meadows-data meadows-ecs
                      meadows-world doctest::doctest)
meadows_set_warnings(meadows-tests)
add_test(NAME meadows COMMAND meadows-tests)

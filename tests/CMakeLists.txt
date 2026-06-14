add_executable(meadows-tests
    Main.cpp
    AbilitySystemTest.cpp
    AiTest.cpp
    AssetsTest.cpp
    CellLoaderTest.cpp
    CollisionTest.cpp
    CombatTest.cpp
    ConditionTest.cpp
    CookerTest.cpp
    DialogueTest.cpp
    EcsTest.cpp
    EventBusTest.cpp
    FactionsTest.cpp
    FormsTest.cpp
    GameplayAbilityTest.cpp
    GameplayEffectsTest.cpp
    GameplayTagsTest.cpp
    GuidTest.cpp
    InventoryTest.cpp
    JobsTest.cpp
    MovementTest.cpp
    PluginLoaderTest.cpp
    QuestTest.cpp
    ReflectTest.cpp
    ResolverTest.cpp
    SceneStackTest.cpp
    SceneSubmitTest.cpp
    ScriptTest.cpp
    SpawnerTest.cpp
    WorldModelTest.cpp
)
target_link_libraries(meadows-tests PRIVATE meadows meadows-data meadows-ecs
                      meadows-world meadows-runtime meadows-gameplay
                      meadows-script meadows-narrative doctest::doctest)
meadows_set_warnings(meadows-tests)
add_test(NAME meadows COMMAND meadows-tests)

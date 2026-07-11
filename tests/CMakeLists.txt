add_executable(meadows-tests
    Main.cpp
    AbilitySystemTest.cpp
    AfflictionsTest.cpp
    AiTest.cpp
    AnimTest.cpp
    AudioTest.cpp
    AssetsTest.cpp
    BarterTest.cpp
    CellDeltaTest.cpp
    CellLoaderTest.cpp
    CellStreamerTest.cpp
    ChunkOcclusionTest.cpp
    ChunkStreamerTest.cpp
    CollisionTest.cpp
    CombatTest.cpp
    ConcurrentQueueTest.cpp
    ConditionTest.cpp
    CookerTest.cpp
    CsvImportTest.cpp
    CuesSchedulesTest.cpp
    DeathPersistenceTest.cpp
    DerivedStatsTest.cpp
    DialogueTest.cpp
    DrugsTest.cpp
    EcsTest.cpp
    EditorLayoutsTest.cpp
    EditSessionTest.cpp
    EnergyExhaustionTest.cpp
    EquipmentStatsTest.cpp
    EventBusTest.cpp
    FactionsTest.cpp
    FormsTest.cpp
    FrameComposerTest.cpp
    FrustumTest.cpp
    GameClockTest.cpp
    GameplayAbilityTest.cpp
    GameplayEffectsTest.cpp
    GameplayTagsTest.cpp
    GltfMeshTest.cpp
    GraphLayoutTest.cpp
    GuidTest.cpp
    HorizontalFormsTest.cpp
    InjuriesTest.cpp
    InventoryTest.cpp
    InventoryViewTest.cpp
    JobsTest.cpp
    LevelEditorTest.cpp
    LocTextTest.cpp
    MeleeSwingTest.cpp
    MovementTest.cpp
    OffensiveStatsTest.cpp
    PhysicsTest.cpp
    PluginConfigTest.cpp
    PrefabChildSaveTest.cpp
    PrefabTest.cpp
    PluginLoaderTest.cpp
    QuestSaveTest.cpp
    QuestTest.cpp
    ReflectTest.cpp
    ResolverTest.cpp
    ResonanceDecayTest.cpp
    ResultTest.cpp
    ResonanceTest.cpp
    RestTest.cpp
    RngTest.cpp
    SaveFormsTest.cpp
    SaveStateTest.cpp
    SceneStackTest.cpp
    SceneSubmitTest.cpp
    ScreenStackTest.cpp
    ScriptTest.cpp
    SpawnerTest.cpp
    StatsTuningTest.cpp
    StatusBuildupTest.cpp
    SurvivalTest.cpp
    SynthesisTest.cpp
    TerrainCollisionTest.cpp
    TerrainNavigatorTest.cpp
    TerrainNoiseTest.cpp
    TerrainPatchesTest.cpp
    TreeGeneratorTest.cpp
    TriggerSystemTest.cpp
    TypedDamageTest.cpp
    VegetationCollisionTest.cpp
    VertexAoTest.cpp
    WeaponMeshesTest.cpp
    WorldModelTest.cpp
)
target_link_libraries(meadows-tests PRIVATE meadows meadows-data meadows-ecs
                      meadows-world meadows-runtime meadows-gameplay
                      meadows-script meadows-narrative meadows-physics
                      meadows-audio doctest::doctest)
meadows_set_warnings(meadows-tests)
add_test(NAME meadows COMMAND meadows-tests)

# The §2.10 link lock (audit U9-3): links the ENTIRE sim (whole-archive)
# WITHOUT meadows-render — any sim object referencing a render/rhi/GL symbol
# breaks this build. Building it (part of ALL) is the test; running it boots
# a token renderer-free slice.
add_executable(meadows-simlink SimLinkProof.cpp)
target_link_libraries(meadows-simlink PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,meadows-data,meadows-ecs,meadows-gameplay,meadows-world,meadows-script,meadows-narrative>"
)
meadows_set_warnings(meadows-simlink)
add_test(NAME simlink COMMAND meadows-simlink)

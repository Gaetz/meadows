add_executable(meadows-tests
    Main.cpp
    AbilitySystemTest.cpp
    AfflictionsTest.cpp
    AiTest.cpp
    AnimTest.cpp
    AudioTest.cpp
    AssetsTest.cpp
    BarterTest.cpp
    BrainScriptTest.cpp
    BezierTest.cpp
    BiomeMapTest.cpp
    CellDeltaTest.cpp
    CellLoaderTest.cpp
    CellStreamerTest.cpp
    ChunkOcclusionTest.cpp
    ChunkStreamerTest.cpp
    CollisionTest.cpp
    CombatAiTest.cpp
    CombatTest.cpp
    CookedMeshTest.cpp
    CookedTextureTest.cpp
    ConcurrentQueueTest.cpp
    ConditionTest.cpp
    CookerTest.cpp
    CsvImportTest.cpp
    CuesSchedulesTest.cpp
    DeathPersistenceTest.cpp
    DerivedStatsTest.cpp
    DialogueTest.cpp
    DrugsTest.cpp
    DungeonBakeTest.cpp
    DungeonDensityTest.cpp
    DungeonMeshTest.cpp
    DungeonMissionTest.cpp
    DungeonNavTest.cpp
    DungeonRecordsTest.cpp
    DungeonSpaceTest.cpp
    EcsTest.cpp
    EditorLayoutsTest.cpp
    EditSessionTest.cpp
    EnergyExhaustionTest.cpp
    EquipmentStatsTest.cpp
    EventBusTest.cpp
    FactionsTest.cpp
    FollowerFormsTest.cpp
    FollowersTest.cpp
    FormsTest.cpp
    FormIndexTest.cpp
    HeightFieldTest.cpp
    FinalizeTest.cpp
    FineErosionTest.cpp
    FluvialErosionTest.cpp
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
    HydrologyTest.cpp
    InjuriesTest.cpp
    InputActionsTest.cpp
    InputTest.cpp
    InventoryTest.cpp
    InventoryViewTest.cpp
    JobsTest.cpp
    KillZTest.cpp
    FloatersTest.cpp
    RiverGeometryTest.cpp
    WaterSolveTest.cpp
    FireFieldTest.cpp
    WaterSimTest.cpp
    WaterQueryTest.cpp
    AuthoringTest.cpp
    SpawnDiagnosticTest.cpp
    LevelEditorTest.cpp
    LocTextTest.cpp
    MapRasterTest.cpp
    MapRecordsTest.cpp
    MasterNetworkTest.cpp
    MeleeStrikeTest.cpp
    MeleeSwingTest.cpp
    MovementTest.cpp
    OffensiveStatsTest.cpp
    PerceptionTest.cpp
    PhysicsTest.cpp
    PlayerActionTest.cpp
    PluginConfigTest.cpp
    PrefabChildSaveTest.cpp
    PrefabTest.cpp
    PluginLoaderTest.cpp
    QuestSaveTest.cpp
    SpellsTest.cpp
    SpiritJetsTest.cpp
    SpiritFireTest.cpp
    WindFieldTest.cpp
    SpiritWaterMeshTest.cpp
    TerrainBrushTest.cpp
    WaterReadingTest.cpp
    SpiritSourcesTest.cpp
    QuestTest.cpp
    ReflectTest.cpp
    ResolverTest.cpp
    ResonanceDecayTest.cpp
    ResultTest.cpp
    ResonanceTest.cpp
    RestTest.cpp
    RidingTest.cpp
    RngTest.cpp
    SaveAsyncTest.cpp
    SaveFormsTest.cpp
    SaveStateTest.cpp
    SceneStackTest.cpp
    SceneSubmitTest.cpp
    SkillsTest.cpp
    SoundResolverTest.cpp
    ScreenStackTest.cpp
    ShadowCullTest.cpp
    ScriptTest.cpp
    SpatialIndexTest.cpp
    SpawnerTest.cpp
    StatsTuningTest.cpp
    StatusBuildupTest.cpp
    SurvivalTest.cpp
    SynthesisTest.cpp
    ValidateTest.cpp
    TerrainCollisionTest.cpp
    TerrainNavigatorTest.cpp
    TerrainGenTest.cpp
    TerrainNoiseTest.cpp
    TerrainPatchesTest.cpp
    TerrainRegionsTest.cpp
    TileBakeTest.cpp
    WaterBodiesTest.cpp
    TreeGeneratorTest.cpp
    TriggerSystemTest.cpp
    TypedDamageTest.cpp
    VegetationCollisionTest.cpp
    VegetationScatterTest.cpp
    VertexAoTest.cpp
    WeaponMeshesTest.cpp
    WorldModelTest.cpp
)
target_link_libraries(meadows-tests PRIVATE meadows meadows-data meadows-ecs
                      meadows-world meadows-runtime meadows-gameplay
                      meadows-script meadows-narrative meadows-physics
                      meadows-audio doctest::doctest)
meadows_set_warnings(meadows-tests)
target_precompile_headers(meadows-tests PRIVATE
        <algorithm> <cmath> <cstdio> <cstring> <functional> <memory>
        <optional> <string> <unordered_map> <vector> <glm/glm.hpp>
        <filesystem> <doctest/doctest.h>
        <engine/core/Log.hpp> <engine/ecs/World.hpp>
        <data/plugins/PluginLoader.hpp> <data/plugins/Resolver.hpp>
        <data/forms/FormDatabase.hpp> <data/forms/FormTypeRegistry.hpp>
        <data/forms/CoreForms.hpp> <gameplay/ability/AbilitySystem.hpp>
        <gameplay/ability/GameplayEffects.hpp>
        <world/scene/Components.hpp> <world/worldspace/WorldForms.hpp>)
# Main.cpp defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN before the include.
set_source_files_properties(Main.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
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

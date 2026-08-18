# Revue de code complète — août 2026

> Suivi de la revue fichier par fichier demandée le 2026-08-14.
> Objectifs : code minimal et compréhensible, machines à états (enum class)
> plutôt que chaînes de if, pas de bloat ni d'initialisations inutiles,
> commentaires justes, ordre des membres (cache), bonnes pratiques C++.
> Les tests headless valident chaque lot. Les notes d'architecture sont
> collectées en fin de document (chantier séparé, NE PAS attaquer pendant
> la revue). Les tests (`tests/`) seront passés en revue en dernier, en
> passe allégée.

Légende : `[x]` revu (RAS ou correctifs mineurs), `[~]` revu avec correctifs notables (voir §Notes), `[ ]` à faire.

## data (44 fichiers)

- [x] `data/editor/EditorLayouts.cpp`
- [x] `data/editor/EditorLayouts.hpp`
- [x] `data/editor/GraphLayout.cpp`
- [x] `data/editor/GraphLayout.hpp`
- [x] `data/forms/AnimForms.cpp`
- [x] `data/forms/AnimForms.hpp`
- [x] `data/forms/AudioForms.cpp`
- [x] `data/forms/AudioForms.hpp`
- [x] `data/forms/CoreForms.cpp`
- [x] `data/forms/CoreForms.hpp`
- [x] `data/forms/Form.hpp`
- [x] `data/forms/FormDatabase.cpp`
- [x] `data/forms/FormDatabase.hpp`
- [x] `data/forms/FormQuery.hpp`
- [x] `data/forms/FormTypeRegistry.hpp`
- [x] `data/forms/LandscapeForms.cpp`
- [x] `data/forms/LandscapeForms.hpp`
- [x] `data/forms/LocForms.cpp`
- [x] `data/forms/LocForms.hpp`
- [x] `data/forms/UiForms.cpp`
- [x] `data/forms/UiForms.hpp`
- [x] `data/forms/VisualForms.cpp`
- [x] `data/forms/VisualForms.hpp`
- [x] `data/plugins/BinaryFormat.cpp`
- [x] `data/plugins/BinaryFormat.hpp`
- [x] `data/plugins/CsvImport.cpp`
- [x] `data/plugins/CsvImport.hpp`
- [x] `data/plugins/EditSession.cpp`
- [x] `data/plugins/EditSession.hpp`
- [x] `data/plugins/PluginConfig.cpp`
- [x] `data/plugins/PluginConfig.hpp`
- [x] `data/plugins/PluginLoader.cpp`
- [x] `data/plugins/PluginLoader.hpp`
- [x] `data/plugins/Record.hpp`
- [x] `data/plugins/RecordDiff.cpp`
- [x] `data/plugins/RecordDiff.hpp`
- [x] `data/plugins/Resolver.cpp`
- [x] `data/plugins/Resolver.hpp`
- [x] `data/plugins/Synthesis.cpp`
- [x] `data/plugins/Synthesis.hpp`
- [x] `data/plugins/TomlWriter.cpp`
- [x] `data/plugins/TomlWriter.hpp`
- [x] `data/plugins/Validate.cpp`
- [x] `data/plugins/Validate.hpp`

## engine (racine) (5 fichiers)

- [x] `engine/Engine.cpp`
- [x] `engine/Engine.hpp`
- [x] `engine/FrameContext.hpp`
- [x] `engine/Game.cpp`
- [x] `engine/Game.hpp`

## engine/anim (2 fichiers)

- [x] `engine/anim/Anim.cpp`
- [x] `engine/anim/Anim.hpp`

## engine/assets (18 fichiers)

- [x] `engine/assets/AssetDatabase.cpp`
- [x] `engine/assets/AssetDatabase.hpp`
- [x] `engine/assets/CookedMesh.cpp`
- [x] `engine/assets/CookedMesh.hpp`
- [~] `engine/assets/CookedTexture.cpp`
- [x] `engine/assets/CookedTexture.hpp`
- [~] `engine/assets/GltfMesh.cpp`
- [x] `engine/assets/GltfMesh.hpp`
- [x] `engine/assets/Image.cpp`
- [x] `engine/assets/Image.hpp`
- [x] `engine/assets/MeshData.hpp`
- [~] `engine/assets/MeshSimplify.cpp`
- [x] `engine/assets/MeshSimplify.hpp`
- [x] `engine/assets/StbImpl.cpp`
- [x] `engine/assets/VertexAo.cpp`
- [x] `engine/assets/VertexAo.hpp`
- [~] `engine/assets/VertexAoCache.cpp`
- [x] `engine/assets/VertexAoCache.hpp`

## engine/audio (2 fichiers)

- [x] `engine/audio/Audio.cpp`
- [x] `engine/audio/Audio.hpp`

## engine/core (16 fichiers)

- [x] `engine/core/Assert.hpp`
- [x] `engine/core/Bezier.cpp`
- [x] `engine/core/Bezier.hpp`
- [x] `engine/core/Clock.hpp`
- [x] `engine/core/ConcurrentQueue.hpp`
- [x] `engine/core/Defines.hpp`
- [x] `engine/core/FrameProbe.hpp`
- [x] `engine/core/Guid.cpp`
- [x] `engine/core/Guid.hpp`
- [x] `engine/core/Hash.hpp`
- [x] `engine/core/Jobs.cpp`
- [x] `engine/core/Jobs.hpp`
- [x] `engine/core/Log.cpp`
- [x] `engine/core/Log.hpp`
- [x] `engine/core/Result.hpp`
- [x] `engine/core/Rng.hpp`

## engine/dungeon (12 fichiers)

- [x] `engine/dungeon/DensityField.cpp`
- [x] `engine/dungeon/DensityField.hpp`
- [x] `engine/dungeon/DungeonBake.cpp`
- [x] `engine/dungeon/DungeonBake.hpp`
- [x] `engine/dungeon/MeshExtract.cpp`
- [x] `engine/dungeon/MeshExtract.hpp`
- [x] `engine/dungeon/MissionGraph.cpp`
- [x] `engine/dungeon/MissionGraph.hpp`
- [x] `engine/dungeon/NavGrid.cpp`
- [x] `engine/dungeon/NavGrid.hpp`
- [x] `engine/dungeon/SpaceGraph.cpp`
- [x] `engine/dungeon/SpaceGraph.hpp`

## engine/ecs (2 fichiers)

- [x] `engine/ecs/World.cpp`
- [x] `engine/ecs/World.hpp`

## engine/fx (2 fichiers)

- [x] `engine/fx/Particles.cpp`
- [x] `engine/fx/Particles.hpp`

## engine/nav (1 fichiers)

- [x] `engine/nav/Nav.hpp`

## engine/physics (2 fichiers)

- [~] `engine/physics/Physics.cpp`
- [x] `engine/physics/Physics.hpp`

## engine/platform (10 fichiers)

- [x] `engine/platform/GlContext.hpp`
- [x] `engine/platform/Input.hpp`
- [x] `engine/platform/Paths.hpp`
- [x] `engine/platform/VulkanSurface.hpp`
- [x] `engine/platform/Window.hpp`
- [x] `engine/platform/common/GlContext.cpp`
- [x] `engine/platform/common/Input.cpp`
- [x] `engine/platform/common/Paths.cpp`
- [x] `engine/platform/common/VulkanSurface.cpp`
- [x] `engine/platform/common/Window.cpp`

## engine/reflect (6 fichiers)

- [x] `engine/reflect/Reflect.hpp`
- [x] `engine/reflect/Registry.cpp`
- [x] `engine/reflect/Registry.hpp`
- [x] `engine/reflect/ValueText.cpp`
- [x] `engine/reflect/ValueText.hpp`
- [x] `engine/reflect/Visit.hpp`

## engine/render (71 fichiers)

- [x] `engine/render/AtmosphereParams.hpp`
- [x] `engine/render/Camera2D.hpp`
- [x] `engine/render/Camera3D.hpp`
- [x] `engine/render/FlyCamera.cpp`
- [x] `engine/render/FlyCamera.hpp`
- [x] `engine/render/FrameComposer.cpp`
- [x] `engine/render/FrameComposer.hpp`
- [x] `engine/render/Frustum.hpp`
- [x] `engine/render/GpuProbe.cpp`
- [x] `engine/render/GpuProbe.hpp`
- [x] `engine/render/MeshBuilder.cpp`
- [x] `engine/render/MeshBuilder.hpp`
- [x] `engine/render/MeshCache.cpp`
- [x] `engine/render/MeshCache.hpp`
- [x] `engine/render/MeshVertexLayout.hpp`
- [x] `engine/render/Projection.hpp`
- [x] `engine/render/ResidencyCache.hpp`
- [x] `engine/render/SceneView.hpp`
- [x] `engine/render/ShaderLibrary.cpp`
- [x] `engine/render/ShaderLibrary.hpp`
- [x] `engine/render/SpriteRenderer.cpp`
- [x] `engine/render/SpriteRenderer.hpp`
- [x] `engine/render/TextureCache.cpp`
- [x] `engine/render/TextureCache.hpp`
- [x] `engine/render/WorldRenderer.cpp`
- [x] `engine/render/WorldRenderer.hpp`
- [x] `engine/render/landscape/ChunkOcclusion.cpp`
- [x] `engine/render/landscape/ChunkOcclusion.hpp`
- [x] `engine/render/landscape/ChunkStreamer.hpp`
- [x] `engine/render/landscape/FarTerrain.cpp`
- [x] `engine/render/landscape/FarTerrain.hpp`
- [x] `engine/render/landscape/FrameUniforms.hpp`
- [x] `engine/render/landscape/FxInstance.hpp`
- [x] `engine/render/landscape/FxRenderer.cpp`
- [x] `engine/render/landscape/FxRenderer.hpp`
- [x] `engine/render/landscape/GpuOcclusion.cpp`
- [x] `engine/render/landscape/GpuOcclusion.hpp`
- [x] `engine/render/landscape/GrassSpecies.hpp`
- [x] `engine/render/landscape/GrassSystem.cpp`
- [x] `engine/render/landscape/GrassSystem.hpp`
- [x] `engine/render/landscape/LightClusters.cpp`
- [x] `engine/render/landscape/LightClusters.hpp`
- [x] `engine/render/landscape/MistMap.cpp`
- [x] `engine/render/landscape/MistMap.hpp`
- [x] `engine/render/landscape/NoiseVolume.cpp`
- [x] `engine/render/landscape/NoiseVolume.hpp`
- [x] `engine/render/landscape/PostFx.cpp`
- [x] `engine/render/landscape/PostFx.hpp`
- [x] `engine/render/landscape/RadianceCascades.cpp`
- [x] `engine/render/landscape/RadianceCascades.hpp`
- [x] `engine/render/landscape/ShadowMapper.cpp`
- [x] `engine/render/landscape/ShadowMapper.hpp`
- [x] `engine/render/landscape/SkySystem.cpp`
- [x] `engine/render/landscape/SkySystem.hpp`
- [x] `engine/render/landscape/SpaceColonizationTree.cpp`
- [x] `engine/render/landscape/SplatTextures.cpp`
- [x] `engine/render/landscape/SplatTextures.hpp`
- [x] `engine/render/landscape/TerrainLightMap.cpp`
- [x] `engine/render/landscape/TerrainLightMap.hpp`
- [x] `engine/render/landscape/TerrainNoise.cpp`
- [x] `engine/render/landscape/TerrainNoise.hpp`
- [x] `engine/render/landscape/TerrainShadeMap.cpp`
- [x] `engine/render/landscape/TerrainShadeMap.hpp`
- [x] `engine/render/landscape/TerrainSystem.cpp`
- [x] `engine/render/landscape/TerrainSystem.hpp`
- [x] `engine/render/landscape/TreeGenerator.cpp`
- [x] `engine/render/landscape/TreeGenerator.hpp`
- [x] `engine/render/landscape/VegetationSystem.cpp`
- [x] `engine/render/landscape/VegetationSystem.hpp`
- [x] `engine/render/landscape/WaterSystem.cpp`
- [x] `engine/render/landscape/WaterSystem.hpp`

## engine/rhi (17 fichiers)

- [x] `engine/rhi/CommandBuffer.hpp`
- [x] `engine/rhi/Device.cpp`
- [x] `engine/rhi/Device.hpp`
- [x] `engine/rhi/Rhi.hpp`
- [x] `engine/rhi/UniqueHandle.cpp`
- [x] `engine/rhi/UniqueHandle.hpp`
- [x] `engine/rhi/backends/gl/GlConvert.hpp`
- [x] `engine/rhi/backends/gl/GlDevice.cpp`
- [x] `engine/rhi/backends/gl/GlDevice.hpp`
- [x] `engine/rhi/backends/gl/GlDevice41.cpp`
- [x] `engine/rhi/backends/gl/GlDevice41.hpp`
- [x] `engine/rhi/backends/gl/GlDevice46.cpp`
- [x] `engine/rhi/backends/gl/GlDevice46.hpp`
- [x] `engine/rhi/backends/gl/GlDeviceBase.cpp`
- [x] `engine/rhi/backends/gl/GlDeviceBase.hpp`
- [x] `engine/rhi/backends/vulkan/VulkanDevice.cpp`
- [x] `engine/rhi/backends/vulkan/VulkanDevice.hpp`

## engine/terrain (27 fichiers)

- [x] `engine/terrain/BiomeMap.hpp`
- [x] `engine/terrain/HeightPatches.hpp`
- [x] `engine/terrain/Noise.hpp`
- [x] `engine/terrain/RiverGeometry.cpp`
- [x] `engine/terrain/RiverGeometry.hpp`
- [x] `engine/terrain/SandboxTerrain.hpp`
- [x] `engine/terrain/TerrainBase.hpp`
- [x] `engine/terrain/WaterBodies.cpp`
- [x] `engine/terrain/WaterBodies.hpp`
- [x] `engine/terrain/WaterInfoMap.cpp`
- [x] `engine/terrain/WaterInfoMap.hpp`
- [x] `engine/terrain/generation/Authoring.cpp`
- [x] `engine/terrain/generation/Authoring.hpp`
- [x] `engine/terrain/generation/Finalize.cpp`
- [x] `engine/terrain/generation/Finalize.hpp`
- [x] `engine/terrain/generation/FineErosion.cpp`
- [x] `engine/terrain/generation/FineErosion.hpp`
- [x] `engine/terrain/generation/FluvialErosion.cpp`
- [x] `engine/terrain/generation/FluvialErosion.hpp`
- [x] `engine/terrain/generation/Hydrology.cpp`
- [x] `engine/terrain/generation/Hydrology.hpp`
- [x] `engine/terrain/generation/TerrainGen.cpp`
- [x] `engine/terrain/generation/TerrainGen.hpp`
- [x] `engine/terrain/generation/ThermalErosion.cpp`
- [x] `engine/terrain/generation/ThermalErosion.hpp`
- [x] `engine/terrain/generation/TileBake.cpp`
- [x] `engine/terrain/generation/TileBake.hpp`

## engine/ui (4 fichiers)

- [x] `engine/ui/ImGuiLayer.cpp`
- [x] `engine/ui/ImGuiLayer.hpp`
- [x] `engine/ui/UiSystem.cpp`
- [x] `engine/ui/UiSystem.hpp`

## game (racine) (36 fichiers)

- [x] `game/AllForms.cpp`
- [x] `game/AllForms.hpp`
- [x] `game/Barter.cpp`
- [x] `game/Barter.hpp`
- [x] `game/InputActions.cpp`
- [x] `game/InputActions.hpp`
- [x] `game/InventoryView.cpp`
- [x] `game/InventoryView.hpp`
- [x] `game/LevelEditor.cpp`
- [x] `game/LevelEditor.hpp`
- [x] `game/MapRaster.cpp`
- [x] `game/MapRaster.hpp`
- [x] `game/RendererAssets.cpp`
- [x] `game/RendererAssets.hpp`
- [x] `game/SaveGame.cpp`
- [x] `game/SaveGame.hpp`
- [x] `game/Scene.hpp`
- [x] `game/SceneStack.cpp`
- [x] `game/SceneStack.hpp`
- [x] `game/SceneSubmit.cpp`
- [x] `game/SceneSubmit.hpp`
- [x] `game/ScreenStack.cpp`
- [x] `game/ScreenStack.hpp`
- [x] `game/Settings.cpp`
- [x] `game/Settings.hpp`
- [x] `game/SoundResolver.cpp`
- [x] `game/SoundResolver.hpp`
- [x] `game/TerrainBakeStreamer.cpp`
- [x] `game/TerrainBakeStreamer.hpp`
- [x] `game/TerrainCollision.cpp`
- [x] `game/TerrainCollision.hpp`
- [x] `game/VegetationCollision.cpp`
- [x] `game/VegetationCollision.hpp`
- [x] `game/WeaponMeshes.cpp`
- [x] `game/WeaponMeshes.hpp`
- [x] `game/main.cpp`

## game/scenes (62 fichiers)

- [x] `game/scenes/CombatArenaScene.cpp`
- [x] `game/scenes/CombatArenaScene.hpp`
- [x] `game/scenes/DungeonGenTool.cpp`
- [x] `game/scenes/DungeonGenTool.hpp`
- [x] `game/scenes/EditorScene.cpp`
- [x] `game/scenes/EditorScene.hpp`
- [x] `game/scenes/FollowerController.cpp`
- [x] `game/scenes/FollowerController.hpp`
- [x] `game/scenes/FxDirector.cpp`
- [x] `game/scenes/FxDirector.hpp`
- [x] `game/scenes/GameHud.cpp`
- [x] `game/scenes/GameHud.hpp`
- [x] `game/scenes/InteractionController.cpp`
- [x] `game/scenes/InteractionController.hpp`
- [x] `game/scenes/LandscapeScene.cpp`
- [x] `game/scenes/LandscapeScene.hpp`
- [x] `game/scenes/LandscapeTuning.hpp`
- [x] `game/scenes/LineOfSight.hpp`
- [x] `game/scenes/MapController.cpp`
- [x] `game/scenes/MapController.hpp`
- [x] `game/scenes/NpcCombatController.cpp`
- [x] `game/scenes/NpcCombatController.hpp`
- [x] `game/scenes/NpcDirector.cpp`
- [x] `game/scenes/NpcDirector.hpp`
- [x] `game/scenes/NpcMovement.cpp`
- [x] `game/scenes/NpcMovement.hpp`
- [x] `game/scenes/NpcScheduleController.cpp`
- [x] `game/scenes/NpcScheduleController.hpp`
- [x] `game/scenes/NpcSpawner.cpp`
- [x] `game/scenes/NpcSpawner.hpp`
- [x] `game/scenes/OptionsController.cpp`
- [x] `game/scenes/OptionsController.hpp`
- [x] `game/scenes/PlayerController.cpp`
- [x] `game/scenes/PlayerController.hpp`
- [x] `game/scenes/ProjectileDirector.cpp`
- [x] `game/scenes/ProjectileDirector.hpp`
- [x] `game/scenes/QuestDirector.cpp`
- [x] `game/scenes/QuestDirector.hpp`
- [x] `game/scenes/RenderTuningIo.cpp`
- [x] `game/scenes/RenderTuningIo.hpp`
- [x] `game/scenes/RideController.cpp`
- [x] `game/scenes/RideController.hpp`
- [x] `game/scenes/SaveController.cpp`
- [x] `game/scenes/SaveController.hpp`
- [x] `game/scenes/SceneConsole.cpp`
- [x] `game/scenes/SceneConsole.hpp`
- [x] `game/scenes/SceneEditor.cpp`
- [x] `game/scenes/SceneEditor.hpp`
- [x] `game/scenes/StreamingController.cpp`
- [x] `game/scenes/StreamingController.hpp`
- [x] `game/scenes/TerrainGenTool.cpp`
- [x] `game/scenes/TerrainGenTool.hpp`
- [x] `game/scenes/TerrainSculptTool.cpp`
- [x] `game/scenes/TerrainSculptTool.hpp`
- [x] `game/scenes/TreeCreationScene.cpp`
- [x] `game/scenes/TreeCreationScene.hpp`
- [x] `game/scenes/UiRouter.cpp`
- [x] `game/scenes/UiRouter.hpp`
- [x] `game/scenes/WeatherController.cpp`
- [x] `game/scenes/WeatherController.hpp`
- [x] `game/scenes/WorldDemoScene.cpp`
- [x] `game/scenes/WorldDemoScene.hpp`

## game/ui (35 fichiers)

- [x] `game/ui/AbilityPanel.cpp`
- [x] `game/ui/AbilityPanel.hpp`
- [x] `game/ui/AnimGraphPanel.cpp`
- [x] `game/ui/AnimGraphPanel.hpp`
- [x] `game/ui/AnimPreviewPanel.cpp`
- [x] `game/ui/AnimPreviewPanel.hpp`
- [x] `game/ui/ClipTimelinePanel.cpp`
- [x] `game/ui/ClipTimelinePanel.hpp`
- [x] `game/ui/ConditionBuilder.cpp`
- [x] `game/ui/ConditionBuilder.hpp`
- [x] `game/ui/ConsolePanel.cpp`
- [x] `game/ui/ConsolePanel.hpp`
- [x] `game/ui/DialogueGraphPanel.cpp`
- [x] `game/ui/DialogueGraphPanel.hpp`
- [x] `game/ui/EffectPanel.cpp`
- [x] `game/ui/EffectPanel.hpp`
- [x] `game/ui/EventPicker.cpp`
- [x] `game/ui/EventPicker.hpp`
- [x] `game/ui/FieldWidgets.cpp`
- [x] `game/ui/FieldWidgets.hpp`
- [x] `game/ui/FormPicker.cpp`
- [x] `game/ui/FormPicker.hpp`
- [x] `game/ui/FxPanel.cpp`
- [x] `game/ui/FxPanel.hpp`
- [x] `game/ui/Keywords.cpp`
- [x] `game/ui/Keywords.hpp`
- [x] `game/ui/NodeCanvas.cpp`
- [x] `game/ui/NodeCanvas.hpp`
- [x] `game/ui/OverlayBar.hpp`
- [x] `game/ui/PropertyGrid.cpp`
- [x] `game/ui/PropertyGrid.hpp`
- [x] `game/ui/QuestGraphPanel.cpp`
- [x] `game/ui/QuestGraphPanel.hpp`
- [x] `game/ui/RenderTuningPanels.cpp`
- [x] `game/ui/RenderTuningPanels.hpp`

## gameplay (87 fichiers)

- [x] `gameplay/ability/AbilitySystem.cpp`
- [x] `gameplay/ability/AbilitySystem.hpp`
- [x] `gameplay/ability/Attributes.hpp`
- [x] `gameplay/ability/DerivedStats.hpp`
- [x] `gameplay/ability/GameplayAbility.cpp`
- [x] `gameplay/ability/GameplayAbility.hpp`
- [x] `gameplay/ability/GameplayEffects.cpp`
- [x] `gameplay/ability/GameplayEffects.hpp`
- [x] `gameplay/ability/GameplayTags.cpp`
- [x] `gameplay/ability/GameplayTags.hpp`
- [x] `gameplay/actors/ActorState.hpp`
- [x] `gameplay/actors/CharacterForms.cpp`
- [x] `gameplay/actors/CharacterForms.hpp`
- [x] `gameplay/actors/CharacterTick.cpp`
- [x] `gameplay/actors/CharacterTick.hpp`
- [x] `gameplay/actors/FollowerForms.cpp`
- [x] `gameplay/actors/FollowerForms.hpp`
- [x] `gameplay/actors/Followers.cpp`
- [x] `gameplay/actors/Followers.hpp`
- [x] `gameplay/actors/Riding.cpp`
- [x] `gameplay/actors/Riding.hpp`
- [x] `gameplay/actors/Swimming.cpp`
- [x] `gameplay/actors/Swimming.hpp`
- [x] `gameplay/ai/AiForms.cpp`
- [x] `gameplay/ai/AiForms.hpp`
- [x] `gameplay/ai/ScheduleSystem.cpp`
- [x] `gameplay/ai/ScheduleSystem.hpp`
- [x] `gameplay/combat/Combat.cpp`
- [x] `gameplay/combat/Combat.hpp`
- [x] `gameplay/combat/CombatAi.cpp`
- [x] `gameplay/combat/CombatAi.hpp`
- [x] `gameplay/combat/MeleeStrike.cpp`
- [x] `gameplay/combat/MeleeStrike.hpp`
- [x] `gameplay/combat/MeleeSwing.cpp`
- [x] `gameplay/combat/MeleeSwing.hpp`
- [x] `gameplay/combat/PlayerAction.cpp`
- [x] `gameplay/combat/PlayerAction.hpp`
- [x] `gameplay/combat/Projectile.cpp`
- [x] `gameplay/combat/Projectile.hpp`
- [x] `gameplay/condition/Condition.cpp`
- [x] `gameplay/condition/Condition.hpp`
- [x] `gameplay/cue/GameplayCues.cpp`
- [x] `gameplay/cue/GameplayCues.hpp`
- [x] `gameplay/event/EventBus.cpp`
- [x] `gameplay/event/EventBus.hpp`
- [x] `gameplay/faction/Factions.cpp`
- [x] `gameplay/faction/Factions.hpp`
- [x] `gameplay/interaction/Furniture.cpp`
- [x] `gameplay/interaction/Furniture.hpp`
- [x] `gameplay/interaction/FurnitureForms.cpp`
- [x] `gameplay/interaction/FurnitureForms.hpp`
- [x] `gameplay/inventory/Inventory.cpp`
- [x] `gameplay/inventory/Inventory.hpp`
- [x] `gameplay/save/SaveForms.cpp`
- [x] `gameplay/save/SaveForms.hpp`
- [x] `gameplay/save/SaveState.cpp`
- [x] `gameplay/save/SaveState.hpp`
- [x] `gameplay/stats/Afflictions.cpp`
- [x] `gameplay/stats/Afflictions.hpp`
- [x] `gameplay/stats/CharacterStats.cpp`
- [x] `gameplay/stats/CharacterStats.hpp`
- [x] `gameplay/stats/CoreAttributes.hpp`
- [x] `gameplay/stats/Damage.cpp`
- [x] `gameplay/stats/Damage.hpp`
- [x] `gameplay/stats/Drugs.cpp`
- [x] `gameplay/stats/Drugs.hpp`
- [x] `gameplay/stats/EquipmentStats.cpp`
- [x] `gameplay/stats/EquipmentStats.hpp`
- [x] `gameplay/stats/GameClock.hpp`
- [x] `gameplay/stats/GameTime.cpp`
- [x] `gameplay/stats/GameTime.hpp`
- [x] `gameplay/stats/Injuries.cpp`
- [x] `gameplay/stats/Injuries.hpp`
- [x] `gameplay/stats/Resonance.cpp`
- [x] `gameplay/stats/Resonance.hpp`
- [x] `gameplay/stats/ResonanceDecays.cpp`
- [x] `gameplay/stats/ResonanceDecays.hpp`
- [x] `gameplay/stats/Rest.cpp`
- [x] `gameplay/stats/Rest.hpp`
- [x] `gameplay/stats/Skills.cpp`
- [x] `gameplay/stats/Skills.hpp`
- [x] `gameplay/stats/StatsTuning.cpp`
- [x] `gameplay/stats/StatsTuning.hpp`
- [x] `gameplay/stats/StatusBuildup.cpp`
- [x] `gameplay/stats/StatusBuildup.hpp`
- [x] `gameplay/stats/Survival.cpp`
- [x] `gameplay/stats/Survival.hpp`

## quest (4 fichiers)

- [x] `quest/Dialogue.cpp`
- [x] `quest/Dialogue.hpp`
- [x] `quest/Quest.cpp`
- [x] `quest/Quest.hpp`

## script (4 fichiers)

- [x] `script/ScriptVars.cpp`
- [x] `script/ScriptVars.hpp`
- [x] `script/Vm.cpp`
- [x] `script/Vm.hpp`

## tools (4 fichiers)

- [x] `tools/cooker/Main.cpp`
- [x] `tools/cooker/TextureCook.cpp`
- [x] `tools/cooker/TextureCook.hpp`
- [x] `tools/vksmoke/Main.cpp`

## world (51 fichiers)

- [x] `world/ai/AiController.cpp`
- [x] `world/ai/AiController.hpp`
- [x] `world/ai/GridNavigator.cpp`
- [x] `world/ai/GridNavigator.hpp`
- [x] `world/ai/InteriorNavigator.cpp`
- [x] `world/ai/InteriorNavigator.hpp`
- [x] `world/ai/Pathfinding.cpp`
- [x] `world/ai/Pathfinding.hpp`
- [x] `world/ai/Perception.cpp`
- [x] `world/ai/Perception.hpp`
- [x] `world/ai/Steering.hpp`
- [x] `world/ai/TerrainNavigator.cpp`
- [x] `world/ai/TerrainNavigator.hpp`
- [x] `world/dungeon/DungeonRecords.cpp`
- [x] `world/dungeon/DungeonRecords.hpp`
- [x] `world/scene/AnimBridge.cpp`
- [x] `world/scene/AnimBridge.hpp`
- [x] `world/scene/Collision.cpp`
- [x] `world/scene/Collision.hpp`
- [x] `world/scene/Components.cpp`
- [x] `world/scene/Components.hpp`
- [x] `world/scene/Floaters.cpp`
- [x] `world/scene/Floaters.hpp`
- [x] `world/scene/KillZ.cpp`
- [x] `world/scene/KillZ.hpp`
- [x] `world/scene/Movement.cpp`
- [x] `world/scene/Movement.hpp`
- [x] `world/scene/SpatialIndex.cpp`
- [x] `world/scene/SpatialIndex.hpp`
- [x] `world/scene/Spawner.cpp`
- [x] `world/scene/Spawner.hpp`
- [x] `world/scene/TriggerSystem.cpp`
- [x] `world/scene/TriggerSystem.hpp`
- [x] `world/streaming/CellLoader.cpp`
- [x] `world/streaming/CellLoader.hpp`
- [x] `world/streaming/CellStreamer.cpp`
- [x] `world/streaming/CellStreamer.hpp`
- [x] `world/terrain/BiomeMapBuilder.cpp`
- [x] `world/terrain/BiomeMapBuilder.hpp`
- [x] `world/terrain/TerrainPatches.cpp`
- [x] `world/terrain/TerrainPatches.hpp`
- [x] `world/terrain/TerrainRegions.cpp`
- [x] `world/terrain/TerrainRegions.hpp`
- [x] `world/terrain/WaterBodiesBuilder.cpp`
- [x] `world/terrain/WaterBodiesBuilder.hpp`
- [x] `world/worldspace/FormCategory.cpp`
- [x] `world/worldspace/FormCategory.hpp`
- [x] `world/worldspace/WorldForms.cpp`
- [x] `world/worldspace/WorldForms.hpp`
- [x] `world/worldspace/WorldModel.cpp`
- [x] `world/worldspace/WorldModel.hpp`

## Notes de revue (correctifs notables)

- `engine/core/FrameProbe.hpp` — `<cstdio>` manquant pour `std::snprintf` (inclusion transitive). Ajouté. Reste du module : RAS.
- `engine/platform/common/Input.cpp` — tag `[cpp-tuning]` en commentaire (contraire à la politique de commentaires §8) → `Hand-tuned.`. Reste platform/reflect/ecs/nav : RAS.
- `engine/physics/Physics.cpp` — tags de plan « P0 D2b » dans deux commentaires → retirés. Reste : RAS.
- `engine/anim/Anim.cpp` (observation, pas de changement) — au wrap d'une boucle, les événements situés entre 0 et le temps replié sont émis à la frame SUIVANTE (previous repart du temps replié) ; un événement placé pile au début du clip peut glisser d'une frame. Négligeable aux dt courants ; à corriger seulement si un hit-window anim le révèle.
- Racine engine/ (Engine, Game, FrameContext), anim, audio, fx : RAS.
- `engine/assets/CookedTexture.cpp` — le payload était redimensionné sans borne de validité (un en-tête corrompu pouvait demander des Go) ; garde ajoutée, alignée sur le `kMaxCounts` de CookedMesh.
- `engine/assets/MeshSimplify` + `GltfMesh` — `normalizeMeshFootprint` dupliquait `normalizeMesh` (même algo, epsilon près) ; supprimé, l'appelant (LandscapeScene) pointe sur `normalizeMesh`.
- `engine/assets/GltfMesh.cpp` — deux wrappers RAII cgltf et le boilerplate parse/load répété 5× ; consolidé en `ParsedGltf` + `parseWithBuffers`/`parseFromMemory` (~60 lignes en moins, logs d'erreur uniformisés avec code cgltf).
- `engine/assets/VertexAoCache.cpp` — la variante content-keyed dupliquait la validation d'en-tête et l'écriture ; factorisé en `readEntry`/`writeEntry` partagés (~25 lignes en moins). Tests : 645 verts.
- `engine/ui` (ImGuiLayer, UiSystem) : RAS — les adaptateurs RHI sont propres, les setters de modèle se ressemblent mais portent des types différents (les templatiser obscurcirait).
- `engine/rhi` (interface + backend GL complet) : RAS — la séparation GlDeviceBase / 46 / 41 est nette, l'anti-leak de setPipeline est bien tenu.

## Notes d'architecture (à ne PAS traiter pendant la revue)

### Passes par agents de lecture (2026-08-14, après vérification manuelle des constats)

Modules relus fichier par fichier par agents (critères identiques), constats
vérifiés puis appliqués par la session principale. `gameplay/` : rapport en
attente. `tests/` : passe allégée à faire.

**Correctifs appliqués (bugs)**
- `VulkanDevice.cpp` — interblocage latent timeline gfx/upload (signal conditionnel
  vs `gfxDoneValue` inconditionnel) ; `vkDeviceWaitIdle` AVANT le find/chemin différé
  dans destroyTexture (+ 4 destroy) ; fuite de VkImageView sur échec de
  createFramebuffer ; duplication updateBuffer factorisée (`stageIntoFrameCb`) ;
  destructions imbriquées sous le mauvais garde ; 3 commentaires périmés/tags.
- `grass.vert` — clamp d'espèce 0..3 au lieu de 0..5 : Moss/Lichen rendus comme
  Flower (largeur/teinte). Corrigé — À VALIDER VISUELLEMENT en jeu.
- `tree.frag` — index `uLeafSeason[8]` hors bornes au bord du slot 7. Clampé.
- `PostFx.cpp` — `kSsaoShader` + 2 computes froxel absents de `kPassShaders` :
  hot-reload de ces shaders sans effet. Ajoutés.
- `WaterSystem.cpp` — refreshPipeline ignorait `waterlocal` (hot-reload mort) ;
  destroy() ne réinitialisait pas l'état de fraîcheur (pool map placeholder après
  destroy+create). Corrigés.
- `FarTerrain.cpp` — `++generation` annulé par `*this = {}`. Préservé à travers le reset.
- `GpuProbe.cpp` — retour de `timestampReady` non testé (échantillon compute pas prêt
  = horloge absolue dans les stats + fuite) + code mort. Corrigés.
- `RadianceCascades.cpp` — UBO `RcUniforms` non initialisé (octets de pile uploadés). `{}`.
- `TerrainNoise.cpp` — garde maskHeight manquante (div par zéro théorique).
- `FluvialErosion.cpp` — copie intégrale de la grille de drainage à CHAQUE itération
  (80× par bake). Sortie de boucle + move.
- `NavGrid.cpp` — `levelCount` lu du fichier non borné avant resize (bad_alloc sur
  cache corrompu). Borné.
- `DungeonBake.cpp` — break manquant dans la recherche leverOverrides.
- `SpaceGraph.cpp` — identifiants `far`/`near` (macros Windows). Renommés.
- `MeshExtract.cpp` — 3 gardes tautologiques supprimés.
- `CookedTexture/GltfMesh/MeshSimplify/VertexAoCache` (déjà consignés plus haut).

**Corrigés aussi** : commentaires hors politique (dates, « dev decision/retour dev »,
anciennes valeurs, noms de branche, faux « no SSAO », « tomorrow » périmés) dans
ShadowMapper/VegetationSystem/TerrainSystem/SplatTextures/TreeGenerator/PostFx/
NoiseVolume/WorldRenderer/temporal_resolve/terrain_zones/ssdm_common/tree.frag/
FluvialErosion + data/ (tags « horizontal pass H* », « post-7/07 »). NOTE
conventions : `[cpp-tuning]` est une convention DOCUMENTÉE (StatsTuning.hpp) — ne
pas y toucher ; les ancres « U3-x », « PG1/PG3 », « D1..D7 » (DUNGEON-GEN),
« V1..V4 » (RENDERING/VOLUMETRIC) sont des renvois de doc légitimes.

**Bugs supplémentaires APPLIQUÉS dans cette session (vague 2)**
- gameplay : `GameplayEffects.cpp` — `attribute2` recevait un effectId distinct
  (removeEffectById laissait le malus secondaire infini) → même id que la ligne
  primaire ; NOTE : à travers une sauvegarde, restoreActiveEffect réalloue un id
  par ligne — si le groupage doit survivre au save, effectId doit rejoindre
  SavedEffectForm. `GameTime.cpp` — les blessures guérissaient en temps de jeu
  BRUT ; désormais gated sur `combat.restSeconds > 0` (docs/STATS.md §5 :
  récupération au REPOS seulement) — CHANGEMENT DE COMPORTEMENT GAMEPLAY à
  valider par le dev. `updateExhaustion` : le commentaire d'en-tête disait
  « current energy », le code lit la BASE — commentaire aligné sur le code
  (à trancher si le design voulait l'inverse).
- `game/RendererAssets.cpp` — garde `find("_diff_")` (out_of_range sur nom moddé).
- `game/ui/EventPicker.cpp` — test null dans ensureNodeEvent.
- `game/scenes/RenderTuningIo.cpp` — les 9 champs conifère/feuille manquants de
  captureTreeTuning ajoutés (Save type sauvait les anciennes valeurs).
- `game/scenes/TerrainSculptTool.cpp` — chunkSize = kChunkSize.
- Casts float négatif → u32 (graines cosmétiques) routés par i32 : FxDirector ×4,
  CombatArenaScene ×2, LandscapeScene ×2.
- `game/scenes/FollowerController.cpp` — dismiss/bury utilisent groundAt
  (interiorMode) au lieu du height field brut.
- `world/dungeon/DungeonRecords.cpp` — break + invariant « une barrière par
  verrou » documenté (collision de guid sinon).
- `tools/cooker/TextureCook.cpp` — le static-accumulateur devient une locale
  hoistée avant la boucle de layers.
- `world/scene/Components.cpp` — WaterVolume enregistré dans le pont réflexion.
- `world/terrain/TerrainRegions` — doc/logs TRG1 → TRG2 + 6 canaux.
- `engine/terrain` — includes <algorithm> manquants (Thermal/FineErosion) ;
  Finalize.cpp : catmullRom local remplacé par terrain::catmullRom ;
  WaterInfoMap : les 5 tableaux rivières ne s'allouent plus sans rivières ;
  TileBake : la garde `gentle` ne conditionne plus la lithologie (piège latent).
- gameplay micros : Damage.hpp forwards redondants ; CharacterTick includes ;
  StatusBuildup <iterator> + static_assert kRows/StatusType + branche vide ;
  ResonanceDecays hoist des attr() ; GameplayEffects parseOp réutilisé ;
  logs « É6: » nettoyés (gameplay/Followers) ; GameTime.hpp titre en anglais.
- `game/scenes/LandscapeScene.cpp` — include Paths dupliqué + <ctime> mort ;
  TreeCreationScene <cstring>→<filesystem> ; TerrainBakeStreamer <cstring> ;
  SaveGame vecteur mort.

**Étape 1 du backlog APPLIQUÉE (2026-08-15)** : ScreenStack — overlays en
ordre d'empilement (vector, re-show garde la position, test doctest) ;
DungeonGenTool — écrire-tout-puis-enregistrer + LOG_ERROR (échec = seed
retentable, rien d'enregistré) ; gardes SaveController(notify),
UiRouter(barterTrade + LOG_WARN), SceneEditor(isA sur le downcast
worldspace ; near/far → tNear/tFar), GameHud(has<AbilitySystem>).
651 tests verts. **Étape 2 APPLIQUÉE (2026-08-15)** :
- `SaveState::applySavedState` — `initializeCurrent` retiré : il écrasait
  les maxima DÉRIVÉS (formules) avec les graines brutes de l'AttributeSet
  jusqu'au tick suivant (HUD faux 1 frame, clamp contre le mauvais max).
  Le recompute partiel suffit — les non-dérivés se resèment des bases
  restaurées, les cibles dérivées gardent leurs valeurs de formule
  (initializeActorStats tourne à CHAQUE spawn avant la branche saved-state,
  le cache derivedTargetIds est donc toujours là). Test dédié « derived
  maxima survive applySavedState », VALIDÉ PAR MUTATION (échoue avec
  l'ancien code). À confirmer en jeu : save→load, HUD correct dès la
  première frame.
- `updateWarmup` — en fait PURE logique (le voile est le document RmlUi
  « loading », pas de dessin ImGui) : signature `updateWarmup(f32 dt)`,
  appelé en FIN de update() (vrai dt sim, hors gate de pause — le boot se
  déroule derrière le menu), plus depuis drawUi/dt ImGui. À valider en jeu
  sur les trois chemins : boot, entrée sandbox, rattrapage spectateur.
- Warnings « frame spike » (CPU FrameProbe + GPU GpuProbe) supprimés à la
  demande du dev — les sondes continuent d'alimenter le HUD perf
  (endFrame/ligne de log retirés, membres morts nettoyés).
652 tests verts.

**Bugs restant À APPLIQUER**
- `game/scenes/LandscapeScene.cpp` — `updateWarmup()` appelé depuis drawUi() avec
  le dt ImGui : nécessite un DÉCOUPAGE logique (→ update(dt)) / dessin du voile
  (reste drawUi) — pas fait car la fonction mélange les deux et le chemin est
  critique au boot.
- `gameplay/save/SaveState.cpp:370` — `initializeCurrent` écrase les maxima dérivés
  juste avant un recompute partiel (transitoire 1 frame) ; le retrait proposé par
  l'agent risque de laisser les currents dérivés à 0 au premier frame après load —
  À ANALYSER avec recomputeCurrent avant de toucher.
- `game/DungeonGenTool.cpp:142` — échec d'écriture silencieux, état partiellement muté.
- `game/ScreenStack.hpp:48` — `std::set<str> overlays` : ordre de dessin alphabétique.
- micro-gardes : `SaveController.cpp:130` notify non gardé ; `UiRouter.cpp:226`
  get_mut sans has<> ; `SceneEditor.cpp:260` downcast non vérifié ; `SceneEditor.cpp:91`
  locals `near`/`far` ; `GameHud.cpp:307` get<AbilitySystem> sans has<>.

**Étape 3 (perf) APPLIQUÉE (2026-08-15)** — à mesurer au HUD perf en jeu :
- `conditionsPass` → `data::childrenOf` (un bucket d'index parent au lieu
  du scan complet de la FormDatabase, par activation d'ability / PNJ
  schedulé / option de dialogue).
- `EventBus::dispatch` — snapshot des IDS d'abonnement (PODs) au lieu de
  copier les `std::function` ; sémantique : un handler DÉSABONNÉ pendant
  le dispatch n'est plus appelé (documenté dans l'en-tête).
- `GpuOcclusion` — le staging `vector<GpuAabb>` (~4 Mo au plein régime)
  devient un scratch membre réutilisé (GpuAabb déplacé dans le hpp).
- `VegetationSystem` — draw() : UNE traversée de la map (verdict de
  visibilité + cheb + hero-near cachés par chunk) puis les boucles
  (variante × niveau) parcourent un vecteur compact ; la règle de LOD
  unifiée dans `canopyLevel` partagée avec collectDrawCandidates (au
  passage : les deux chemins DIVERGEAIENT — collect donnait le hero-near
  sans jumeau low, draw jamais ; unifié sur la règle de draw) ; sélection
  (vb/ib/count) factorisée dans `levelMesh` (draw/indirect/collect).
  drawDepth garde sa politique propre (proxy d'ombre par variante).
- `TerrainSystem` — draw() et drawDepth() : une seule traversée de la map
  par passe (cull + bucket par LOD dans un scratch membre) au lieu de
  kLodCount traversées.
652 tests verts. Restent en backlog perf : les dédups mécaniques
(WeaponMeshes/WeatherController/RenderTuningIo tables, mouselook,
wrapAngle, game/ui) et les refactors à garde-fou (scatter végétation —
séquence RNG, bake mailbox, GridOps).

**Étape 4 (factorisations mécaniques) APPLIQUÉE (2026-08-15)** — surface
moddable (Forms réfléchis / TOML) intacte : tout est côté C++ mapping.
- `WeatherController` : les 23 paires atmos↔WeatherForm ×3 (capture/
  apply/mix) → une table constexpr `kWeatherLanes` de pointeurs-membres.
- `RenderTuningIo` : `FieldLane<Params,Form,T>` + `applyLanes`/
  `captureLanes` génériques ; tables lobe (14 f32 + 2 i32), colonized
  (36 f32 + 7 i32 + 2 vec3), RC (12 f32 + 3 i32 + 4 bool ; l'enum
  technique reste manuel). Un champ oublié dans UNE des deux directions
  n'est plus possible.
- `InputActions::applyLookInput` : mouselook partagé PlayerController/
  RideController (sensibilité settings, invertY, clamp pitch ±89°).
- `NpcMovement` : `wrapAngle`/`smoothYawToward` partagés (NpcMovement,
  NpcScheduleController, RideController — copie locale supprimée).
- game/ui :
  - `TestActor` (set+system+tags+lastResult+reset) + `drawAttributeTable`
    partagés Ability/EffectPanel (~130 lignes dupliquées supprimées) ;
  - `kWarnColor` unique dans Keywords.hpp (4 constantes locales + 3
    littéraux DialogueGraphPanel supprimés) ;
  - `FormPicker` : drawItemPicker/drawFormPicker → un squelette
    `drawPickerCombo` (prédicat de type + suffixe " (Type)") ;
  - `FieldWidgets::rawTextField` : LE cache d'édition texte unique ;
    PropertyGrid::drawText et textField se rebasent dessus (deux caches
    « un seul actif » concurrents → un seul) ;
  - `GraphPanelCommon` (applyGraphLayout / placePendingNode /
    persistMovedNodes) : le bloc layout ×3 des panneaux graphes
    (anim/quest/dialogue) factorisé — chaque panneau ne garde que sa
    collecte nodes/edges/roots ;
  - DialogueGraphPanel : `nextOrderUnder` + `createReply` partagés par le
    re-parent et les deux popups « + reply » (deux corps identiques).
652 tests verts après chaque famille. À valider en jeu : météo (mix des
23 canaux), save/load des tunings render, mouselook à cheval, éditeurs de
graphes (auto-layout, drag, « + reply »).

**Étape 5 (refactors à garde-fou) APPLIQUÉE (2026-08-15)** — un par un,
chacun validé par build + suite complète (654 tests) :
- **Scatter végétation** — le garde-fou d'abord :
  `tests/VegetationScatterTest.cpp` fige le contrat « l'ordre de tirage
  RNG par candidat est la séquence » : (a) déterminisme + chaque tier
  peuplé sur un échantillon de chunks, (b) hash d'or FNV-1a des buffers
  d'instances bruts (`0xf9368670a9b7b74f`, seed 1337 procédural pur —
  un changement de tuning volontaire le re-capture via le MESSAGE).
  Puis : blocs Plants/Mass fusionnés en une table `kPlantTiers` ×2
  (salt, spacing, fade, variante de base, échelle, acceptances par
  espèce) sur UNE boucle commune — hash d'or inchangé, donc buffers
  bit-identiques. Au passage : les 5 fonctions upload (variant/low/
  ultra/near/caster) rebasées sur `uploadMeshBuffers` (un triple
  vb/ib/count).
- **`BakeMailbox<Baked>`** (`engine/render/landscape/BakeMailbox.hpp`) :
  le pattern « un bake worker en vol, résultats par ConcurrentQueue,
  génération qui orpheline à travers destroy » extrait des 4 copies —
  TerrainLightMap, TerrainShadeMap, MistMap, FarTerrain (create/reset/
  drain/kick/busy/ready). FarTerrain::destroy garde sa subtilité (la
  génération survit au `*this = {}`) via move-out/reset/move-in.
  À valider en jeu : ombres lointaines, shading régions, mist, horizon
  far — rebakes au déplacement + après changement de monde.
- **`GridOps.hpp`** (`engine/terrain/generation/`) : `kNeighbours8`
  (table ×3 des passes d'érosion fine/fluviale/thermique),
  `bilinearGrid`/`bilinearWorld` (×3 : Finalize bilinearGrid + hydroAt,
  TileBake bilinearAt), `chamferSweep` (×3 : distanceToMask, rive de
  lac, signedSeaDistance) — remplacements bit-identiques (même ordre de
  relaxation, mêmes poids). Les deux bilinéaires restants (rive de lac
  non-carrée, canal u8 de TerrainBase) ont des accès différents et
  restent en place.

**Étape 6 (commentaires/tags) APPLIQUÉE (2026-08-15)** — purement
textuelle (aucun changement de code ; build + 654 tests verts) :
- **Préfixes de chantier dans les LOGS supprimés** (décision dev) : les
  ~50 occurrences É1-É10/B1-B9/C2-C9.6/D2a-b/A7/R7/H3 strippées
  (FollowerController, NpcSpawner, NpcCombatController, NpcDirector,
  LandscapeScene, MapController, ProjectileDirector, PlayerController,
  TerrainSculptTool, QuestDirector, KillZ). Le log de transition combat
  gagne un préfixe sémantique (`combat: {} -> {}`) à la place du B3.
- **Sigles de chantier dans les commentaires** (game/scenes, world,
  quest, gameplay) : tags nus (C2/C3, B1-B6, R3/R6/R7, H3/H8, D1/D2,
  8.7c/8.7e, C1) retirés — la phrase porte l'info seule. Les ancres
  QUALIFIÉES par un doc restent (docs/RENDERING.md §5 B0/B1, §7 R3/R5,
  docs/HORIZONTAL-PASS.md §H5, WorldForms H3) ; idem [cpp-tuning],
  U3-x/PG/D1-D7/V1-V4 (conventions documentées).
- **Français → anglais** dans tous les commentaires de code (y compris
  tests) : citations de FOLLOWERS.md traduites en gardant le pointeur
  §, libellés de dialogue/stances remplacés par leurs identifiants
  (Stay, "attack my target", OnLearnPerk…). Restent verbatim : les
  identifiants de data ("Poney" = editorId) et les titres de sections
  de docs français qui servent d'ancre (MEADOWS-PLAN §A « Volumes de
  gameplay », DUNGEON-GEN § « algorithme à tiroirs »).
- **Fragments/faux corrigés** : SceneEditor.hpp (ligne coupée « //. »),
  GridNavigator.hpp (le mapping est X/Y et c'est la composante Z des
  waypoints qui reste à 0 — pas Y), LevelEditor.cpp (justification
  « rehash » fausse : les drafts sont des uptr, pointeurs stables —
  commentaire supprimé), InputActions.cpp (« F was free » historique).

**Étape 7 (passe allégée tests/) APPLIQUÉE (2026-08-15)** — 131 fichiers
relus (5 agents lecteurs, chaque signalement vérifié manuellement) ;
build + 654 tests verts après correctifs :
- **Assertions renforcées** : AfflictionsTest — `.epsilon(1.0f)` (tolérance
  relative de 100 %, le « timer refreshed » ne pouvait pas échouer) →
  epsilon par défaut ; InjuriesTest — `countBefore` calculé jamais vérifié
  (avec un commentaire prétendant le contraire) → `CHECK(countBefore > 0)` ;
  QuestSaveTest — indexation `[1]` gardée par un `REQUIRE(size >= 2)` ;
  DungeonBakeTest — le produit `meshes × torches` (égalité fortuite
  possible) → comparaison par composante ; GraphLayoutTest — deux CHECK
  dupliqués supprimés ; CuesSchedulesTest — première capture de `p1`
  morte (écrasée inconditionnellement) supprimée.
- **Commentaires faux corrigés** : SaveStateTest (« at its authored
  spot » alors que la caisse est déplacée en 999), FrameComposerTest
  (« sun straight ahead » alors que le soleil pointe hors écran — c'est
  ce que le test vérifie), EditSessionTest (commentaire tronqué du undo),
  PlayerActionTest (prose historique « used to grant... now » → règle
  durable), FinalizeTest (index de `plain` via le spec de `fine` —
  même valeur aujourd'hui, corrigé par propreté), DungeonRecordsTest
  (`enemyHandle` qui tient une torche → `movedHandle`).
- **Tags de chantier retirés** des commentaires ET des noms de TEST_CASE
  (~60 : É2-É11 ×46 dans FollowersTest/EquipmentStats/Riding, B1-B6,
  H1-H8, C9.4/C9.5, 8.7c/8.7e, P0 A7/D2b, R5/A5, ch.2, « Brick 3 »,
  « Batch-1 », « brick e », « review 7b », branche
  feature/space-colonization-trees). Conservés : les ancres d'audit
  U5-5/U6-F10/U7-6 et les étages S1-S4 du pipeline terrain (conventions
  documentées) ; les chaînes de DATA (slots « c97-* », dossier
  « meadows-c3 ») ne sont pas des commentaires.
- **Français résiduel traduit** : 3 bannières de section FollowersTest,
  la citation de SpawnDiagnosticTest, TerrainNavigator/FollowersTest
  (déjà à l'étape 6). Divers : `using render::MeshData` mort
  (GltfMeshTest), `(void)cellEntity` mort (FollowersTest).
- **Non traités (style accepté, passe allégée)** : les derefs
  `find(...)->field` sans REQUIRE préalable (HorizontalForms, Resolver,
  PrefabChildSave — un fixture cassé crashe au lieu d'échouer proprement,
  signal suffisant) ; includes en milieu de fichier (Script, GameClock,
  SpawnDiagnostic) ; les `CHECK(true)` des cas diagnostics `skip()` de
  SpawnDiagnosticTest (sortie MESSAGE only, volontaire). Couverture
  notée : le plafond létal de `fallDamage` (cap 30) n'est pas exercé —
  TypedDamageTest passe par `killOutright`.

**LA REVUE EST CLOSE** : production + tests relus, étapes 1-7 appliquées.
Restent au backlog long terme : décisions design STATS (19),
sources d'inflictEffect, petits items perf, décisions dev (AiController
mort, log-prefix tranché : supprimés).


**Simplifications/duplications restant À APPLIQUER (sélection, cf. rapports)**
- ~~WeatherController : table de 23 paires pointeur-membre ×3~~ FAIT (étape 4).
- ~~PlayerController/RideController : mouselook dupliqué~~ FAIT (étape 4).
- ~~NpcMovement/… : wrapAngle/smoothYaw ×3~~ FAIT (étape 4).
- VegetationSystem : ~~5 uploads, sélection LOD, scatter Plants/Mass,
  traversées de map~~ FAIT (étapes 3-5). Reste : `unordered_map<u64,bool>
  visible` par frame (mineur).
- TerrainSystem : draw/drawDepth à factoriser + listes par LOD persistantes.
- RadianceCascades : lambda `snap` dupliquée prepare/update.
- GpuOcclusion : `vector<GpuAabb>` (~4 Mo) réalloué par frame.
- WorldRenderer : slotFor/sweepUnseen ×3 ; keyShadow vectors par frame (type anon —
  nécessite exposer le type) ; `if (cfg.terrain)` consécutifs ; `!rcOnly` dans la
  condition de boucle ; indentation recordGiUpdate.
- Finalize.cpp : ~~chamfer ×3 + bilinéaire ×4 + table 8-voisins ×3~~ FAIT
  (étape 5, GridOps.hpp). Restent : paramètre MacroResult surdimensionné
  (copie ~800 Ko/bake, ne lit que seaDist) ; TileBake garde `gentle`
  trompeuse.
- Hydrology : param `junctionPond` toujours identique ; `cutAt` sentinelle fragile.
- WaterInfoMap : 5 tableaux pleine carte alloués même sans rivières ; noyau rivière
  dupliqué avec WaterBodies::riverFlowSample (commentaire « SHARED KERNEL » faux).
- ~~MistMap/TerrainLightMap/TerrainShadeMap/FarTerrain : bake mailbox ×4~~
  FAIT (étape 5, BakeMailbox.hpp).
- game/ui : ~~drawAttributeTable + test-actor, bloc layout ×3, FormPicker ×2,
  cache d'édition ×2, kWarnColor~~ FAIT (étape 4). Restent : autres couleurs
  partagées éventuelles ; QuestGraphPanel eventHasEmitter O(n²)/frame ;
  InventoryView lowered() dans le tri ; main.cpp table demos par frame ;
  SceneSubmit double requête lumières ; TerrainCollision/VegetationCollision/
  TerrainBakeStreamer pack u64 ×4 ; SaveGame optional/code mort/double has<> ;
  Settings lookups ; vksmoke duplications.
- world : AiController mort (à supprimer avec sa ligne CMake — décision dev) ;
  WaterBodiesBuilder clés u64 écrasant des Guid (collision possible) → clés Guid ;
  TriggerSystem allocations par tick ; Spawner O(prefabs×forms) ; Quest triple
  balayage par événement ; Components.cpp WaterVolume non enregistré ;
  TerrainRegions doc TRG1 vs code TRG2 ; casts avant isA (WorldModel/CellStreamer).
- ~~Commentaires : fragments orphelins, tags de chantier, préfixes de
  chantier dans les LOGS, notes en français, GridNavigator.hpp,
  LevelEditor.cpp, InputActions~~ FAIT (étape 6). vksmoke V1-V4 = ancres
  légitimes (conservées).


### Correctif post-validation : le lit ne soignait pas (2026-08-14)

Symptôme rapporté : dormir dans un lit ne remonte pas la santé. Ce n'était
PAS une régression de la revue : `sleep()` sautait l'horloge de 8 h mais le
`gameDt` des ticks dérive du temps réel × timescale (`GameClock::advance`),
donc la fenêtre dormie n'atteignait JAMAIS `tickGameTime` (régén santé/
essence, expiration des drogues, guérison des blessures). Le journal
Phase-7 (brique 4) documentait le contrat « l'appelant chaîne la valeur de
retour de sleep() dans les systèmes de temps de jeu » — jamais câblé ; et
`advanceGameTime` (« Advance / Sleep buttons ») existait sans aucun appelant.

Fix (§2.11 — réutilisation du time-skip existant) :
- `gameplay::sleepGameTime(clock, args, hours, mods)` (GameTime) : saut
  d'horloge + nuit créditée en repos AVANT le skip (dormir juste après un
  combat guérit quand même) + `advanceGameTime` sur la fenêtre + restau du
  besoin de sommeil APRÈS (une nuit complète réveille à 100).
- `Rest::sleep()` supprimé (chemin parallèle) ; `Rest` ne garde que
  `accrueRest`. `InteractionController::rest()` passe par une closure
  `applySleep` fournie par la scène (l'idiome du contexte) ;
  `LandscapeScene::applySleep` assemble les `GameTimeTickArgs` du joueur ;
  l'assemblage equipMods du tick extrait en `playerEquipMods()` (partagé).
- Tests : RestTest réécrit sur `sleepGameTime` + 2 nouveaux cas (régén de
  santé sur la nuit ; blessures qui guérissent même restSeconds=0).
  647 tests verts. Un DoT peut tuer dans le lit (le résultat `died` suit le
  flux de mort normal via le tag, frame suivante).


### Audit du CharacterStatsPanel purgé (2026-08-15) — mécanismes orphelins réintégrés

Le panneau 2D purgé avec les scènes de démo (commit 4a940f2) était le
DERNIER appelant de plusieurs mécanismes gameplay ; audit complet contre
l'état actuel :

**Réintégrés :**
- `advanceGameTime` → le lit (fait la veille via `sleepGameTime` ; en prime
  l'ordre besoin-de-sommeil/decay du panneau était bogué — il restaurait
  AVANT la décroissance, réveil sous 100 — corrigé au passage).
- `rollInjury`/`injuryBaseChance` → plus AUCUN coup n'infligeait de
  blessure depuis la purge. Réintégré dans `resolveStrikeDamage`
  (MeleeStrike, §2.11 : couvre mêlée joueur, mêlée PNJ et flèches en un
  point) derrière `StrikeContext.rng` (nul = pas de roll — les tests de
  frappe existants gardent leurs nombres). Mapping v1 : canal physique
  dominant → tranchant = Cut, contondant = Bruise (Fracture au-delà de la
  moitié de la barre en un coup — les seuils d'injuryBaseChance) ; partie
  du corps = Torso tant que les tables par partie ([7+] STATS.md) ne sont
  pas écrites. RNG : `combatRng` de la scène, câblé dans PlayerContext /
  NpcContext (déjà là) / ProjectileContext. Test doctest ajouté
  (immunité à onyx ≥ 0, infliction déterministe à onyx -100, pas de roll
  sans RNG). 648 tests verts.

**Transversalité (demande dev) :** la plomberie stats sort de
LandscapeScene — `gameplay::gameTimeArgsFor(entity, ctx)` et
`gameplay::sleepCharacter(entity, clock, hours, ctx, mods)` vivent dans
CharacterTick ; `LandscapeScene::applySleep` ne fait plus que nommer le
joueur et ses equip-mods. (Les trois assemblages d'equip-mods —
scène/NpcDirector/CombatArena — restent distincts : compositions
différentes de primitives déjà partagées, pas des doublons.)

**Encore orphelins (décision design demandée au dev) :**
- `inflictEffect` (maladies/psychoses N3) : aucune source d'infliction
  dans le jeu — il faut décider d'où elles viennent (négligence de survie
  prolongée ? environnement ? types d'ennemis ?) avant de câbler.
- `wait()` (attendre au coin du feu) ne fait passer QUE la survie et le
  repos : les drogues n'expirent pas, les afflictions ne progressent pas
  pendant l'attente. Le design dit « restores nothing — that's what beds
  are for » ; à trancher : router aussi l'attente par advanceGameTime
  (drogues/afflictions/régén) ou seulement l'expiration des effets ?
- Le panneau lui-même (inspecteur résonance/harmonie/buildup/blessures du
  joueur) : outil dev perdu — proposable en revival allégé, transversal
  (entity + tickCtx), derrière une touche debug.


### Restauration du banc de stats + wait routé (2026-08-15)

- `game/ui/CharacterStatsPanel` restauré depuis git (purge 4a940f2) et
  adapté : l'état de démo (gear/effets d'exemple) devient un pointeur
  optionnel — sections de banc masquées quand absent ; les boutons de saut
  de temps passent par les nouveaux `waitGameTime`/`sleepGameTime` (au
  passage, l'ancien bouton Sleep restaurait le besoin AVANT la
  décroissance : réveil sous 100 — plus le cas).
- `game/scenes/StatsScene` restaurée (extraite de l'ancien DemoScenes.cpp,
  fichier dédié), enregistrée dans le menu Demos de main.cpp.
- Le panneau est AUSSI hébergé en jeu : F7 dans LandscapeScene, branché sur
  le joueur réel (tickCtx + playerEquipMods, sans état de démo) — le
  panneau était déjà transversal (entity + ctx), seule sa scène hôte avait
  disparu.
- `wait()` (décision dev) routé par `waitGameTime` = `advanceGameTime` +
  fenêtre créditée en repos, SANS restauration du besoin de sommeil ;
  `sleepGameTime` se compose dessus. `waitCharacter` transversal
  (CharacterTick) + closure applyWait (InteractionContext). Test ajouté :
  une drogue de 4 h expire pendant une attente de 6 h. 649 tests verts.


### Audit de conformité STATS.md (2026-08-15) — voir docs/AUDIT/STATS-CONFORMANCE.md

Rapport complet archivé dans docs/AUDIT/STATS-CONFORMANCE.md. Deux
correctifs critiques appliqués dans la foulée (buildups d'arme jamais
appliqués en 3D → portés par DamageEvent et appliqués dans
resolveStrikeDamage, avec le scaling « status damage » §3 au passage ;
consommables à buildup no-op dans UiRouter → pointeur StatusBuildup
ajouté). 650 tests verts. Le reste = 19 décisions design listées dans
l'audit (mort/inconscience, progression des attributs, attributs par
acteur, energy regen ÷2…) + une liste « le doc est en retard » à reporter
dans STATS.md.

### Notes d'architecture (collectées, à NE PAS traiter pendant la revue)

1. **Vulkan** : ~~uploads de textures synchrones~~ FAIT (2026-08-17 :
   les 3 sites images — mips offline, mip de base, generateMipmaps —
   asynchrones ; en frame, v2 : recordImageUpload — ring de staging par
   slot + CB d'upload de la frame, zéro submit/wait par texture ;
   fallback staging dédié + submit async parqué hors frame et pour la
   conversion R16F. Banc vksmoke : 1,77 → 0,63 ms par texture ; à
   surveiller : le ring garde la taille du pire burst. Le double chemin
   draw/GPU-driven est scellé au passage : cas « cull seal » dans
   vksmoke — verdicts GPU vs référence CPU sur depth connue, qui fige
   AUSSI la marge monde de 16 m des chevaucheurs du near plane).
   ~~Politique de destruction~~ FAIT (UNE politique — tous les destroy*
   parquent dans les pendingX stampés frameCounter, drainés par
   flushPendingFrees ; supprime les vkDeviceWaitIdle INCONDITIONNELS de
   destroySampler/Framebuffer/Shader/Pipeline — un stall GPU à chaque
   hot-reload/rebuild de pipeline EN JEU ; le destructeur draine en
   force après son waitIdle). ~~remapBindings~~ FAIT (SPIRV-Reflect —
   compile de la source ORIGINALE, réflexion = source de vérité du
   layout, shift de classe patché dans les décorations ; parseur texte
   gardé en cross-check sous-ensemble transitoire, corpus entier vert).
   ~~shaderc par étage~~ FAIT (un compilateur process-wide au lieu d'un
   init glslang par étage). RESTE : kFramesInFlight=2 câblé — acté
   comme le bon choix (2 = standard latence/débit) ; paramétrer
   seulement avec l'intention de changer la valeur.
2. **WorldRenderer** : ~~render() ~1200 lignes → découpage par passe~~ FAIT
   (2026-08-17 : 9 méthodes extraites en code-motion pur —
   pumpPipelinesAndRequests/pumpStreaming/recordKeyShadowTiles/
   recordRainOcclusion/recordShadowCascades/recordReflection/
   recordMainPass/recordCopyHizWater/recordPostFx ; render() = 549 lignes,
   la table des matières du frame + les décisions par frame : fit des
   cascades, picks key-shadow, composition des uniforms — déjà pure via
   FrameComposer. Les locals inter-passes circulent en PARAMÈTRES
   explicites, plus en portée partagée). NOTE 2 ENTIÈREMENT CLOSE ; ~~60 membres `*Ui`~~ FAIT (2026-08-17 :
   struct public `RenderTuning` — 60 knobs regroupés, commentaires conservés ;
   les `friend` RESTENT, verdict d'inspection : les panels pilotent aussi les
   sous-systèmes (terrain/vegetation/grass/postFx/RC/gpuProbe) — la note
   surestimait ; bonus : applyTuning/captureTuning de RenderTuningIo devenus
   tables FieldLane ×7 (54 lanes 1:1), seuls clamps/packing Vec4/scalaires de
   sous-système restent manuels) ; ~~casters d'ombre non cullés~~ FAIT
   (2026-08-15 : cull AABB des meshes + sphère des skinnés par cascade/tuile
   key/fenêtre rain, main pass inclus — ShadowCullTest fige le contrat
   kCasterReach ; doc RENDERING.md §3.1) ; ~~meshDraws indexé par position du
   snapshot~~ FAIT (2026-08-17 : stamp `casterMeshesData` posé par la passe
   caster, ENGINE_ASSERT dans drawSceneMeshes — un reorder du snapshot entre
   les deux passes est détecté en debug).
3. **Landscape** : ~~« bake mailbox » ×4~~ FAIT (étape 5,
   BakeMailbox.hpp) ; ~~double chemin draw()/GPU-driven~~ FAIT (cas
   « cull seal » de vksmoke) ; ~~PipelineCache~~ FAIT via
   ShaderLibrary::Watch (2026-08-17, décision dev : prévention avant
   symptôme) — RÈGLE actée : Watch pour tout build consommant PLUSIEURS
   shaders (convertis : PostFx, WaterSystem, FarTerrain, casters de
   WorldRenderer, et les 1:1 de Terrain/Grass/Vegetation/Sky/Fx pour
   l'uniformité) ; les refresh à SLOT 1:1 nom-unique (GpuOcclusion,
   RadianceCascades, LightClusters, mesh/skinned/rain/blit de
   WorldRenderer) restent tels quels — la dérive n'y est pas exprimable
   (gate et rebuild dans la même instruction). NB Watch : un watch VIDE
   ne déclenche jamais — le premier build vient de create().
   ~~Split 4-métiers de VegetationSystem~~ FAIT (2026-08-17, décision
   dev) : 2 351 lignes → 4 TUs par métier — VegetationScatter (475,
   sous golden-test), VegetationAssets (610), VegetationStreaming (252),
   VegetationSystem = rendu (563) ; le header reste entier, les noms de
   shaders promus en static constexpr de classe. ~~SoA par variante du Chunk~~
   MESURÉ ET ARCHIVÉ (2026-08-17, micro-bench du motif mémoire à ring
   rayon 24 / 700 visibles / 6 passes / 26 variantes : AoS 0,32 ms vs
   SoA 0,26 ms par frame — gain ≈ 0,06 ms, borne HAUTE, soit <0,1 % de
   frame) : le refactor ne paie pas son risque d'indices chunk↔lane.
   Rouvrir si kVariantCount grossit nettement (gain linéaire en
   variantes × passes) ou si le ring s'étend.
4. **Terrain** : ~~hillChainWavelength en argument séparé~~ FAIT
   (2026-08-18 : champ MacroParams, défaut 0 = désactivé — l'input de
   synthesizeMacro est UN struct ; TileBake copie la valeur du seam de
   contrôle ; tests bit-verts) ; ~~GridOps à compléter~~ FAIT
   (bicubicGrid — l'ex-sampleGrid de Finalize — et boxBlur3 — l'ex-
   lambda de TileBake — rejoignent voisins/bilinéaire/chamfer ; « bbox »
   examiné : les min/max ad hoc n'ont pas de forme commune, pas
   d'extraction) ; ~~résolution bassin vs mares dug~~ FAIT (les deux
   skips explicites existaient déjà — Finalize 439/517 ; le CONTRAT est
   maintenant pinné au champ Lake::dug) ; RESTE : DensityField linéaire
   en primitives → grille de hachage — bake-time seulement (suites
   donjons : 26,5 s au total, mines actuelles OK) ; déclencheur : gros
   donjons / bond du nombre de primitives.
5. **Audio** : ~~pas d'API stop~~ FAIT (2026-08-18 : play() retourne un
   SoundId (0 = échec, bool-compatible pour les appelants existants) ;
   stop(id, fade) fond au silence puis laisse le reap timed d'update()
   dé-initialiser — le chemin des test tones. Aucun consommateur encore :
   le jour où un cue loopé doit s'arrêter, l'API attend).
6. **Anim** : ~~événement au wrap une frame en retard~~ FAIT
   (2026-08-18 : la frame du wrap teste AUSSI le sliver [0, temps
   wrappé] — un événement tôt dans un clip loopé glissait d'une boucle
   entière ; test AnimTest « loop wrap fires the wrapped sliver »).
7. **game/scenes** : ~~FollowerController routeur épais~~ FAIT
   (2026-08-18 : split par métier, header intact — Core 661 lignes
   (lifecycle, sweep, commandes), FollowerCombat 214 (adoption d'aggro,
   disengage, revive), FollowerSocial 681 (banter/affinité/preview/
   perk/forge/mercenaire/enterrement) ; 5 helpers partagés promus en
   statiques privées, buryFollower devient le membre partagé ; les 43
   cas FollowersTest traversent les trois TUs) ; ~~AiController mort~~
   SUPPRIMÉ (décision dev actée — zéro consommateur) ; ~~guids d'écorce
   en dur~~ FAIT (barkOak/PineDiffuse dans LandscapeTuningForm, défauts
   = scans livrés — moddable §5 comme les arrays terrain) ; RESTENT
   décisions : joueur monté invisible du combat (à trancher AVEC le
   chantier montures — options : cible = la monture, cible = le
   cavalier, ou démonter-sous-le-feu) ; forEachVisible sans index dans
   l'éditeur — le point d'indexation est EditSession, déclencheur : le
   volume de contenu qui rend les panneaux poussifs ; surface 2D
   résiduelle restante (Pathfinding/GridNavigator/Steering, collision
   XY) : GARDÉE — c'est le fallback sanctionné du seam nav (§2.10) et
   les scènes 2D de banc l'utilisent.
8. **quest** : ~~balayages FormDatabase par événement~~ FAIT
   (2026-08-18, décision dev : prévention avant volume) — QuestIndex
   (state→branches, branch→tasks, startEvent→quests) construit une fois
   après résolution, possédé par QuestDirector (rebuild lazy si la
   FormDatabase change, invalidé par reset()) ; les balayages Dialogue
   passent sur data::childrenOf (DialogueNodeForm.parent — le bucket de
   l'étape 3). Suites Quest/Dialogue/QuestSave vertes.
9. **VM Lua** : ~~temps résiduel des coroutines perdu~~ FAIT
   (2026-08-18 : le déficit du tick (remaining <= 0 à la reprise) se
   reporte sur l'attente suivante — les wait(t) chaînés tiennent la
   cadence t au lieu d'arrondir chaque attente à la grille du tick ;
   test ScriptTest « chained waits keep their cadence » : 10×wait(0.05)
   sous tick 0.03 finit en <=18 ticks, pas 20).


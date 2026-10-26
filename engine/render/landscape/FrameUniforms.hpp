#pragma once

#include <cstddef> // offsetof (the std140 layout lock below)

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/render/landscape/FrameLanes.hpp"

namespace render {

// Per-frame constants shared by every landscape shader, uploaded once per
// frame to UBO binding 0. Generated lane by lane from MEADOWS_FRAME_LANES
// (FrameLanes.hpp — the table carries each channel's meaning); the GLSL
// FrameUbo block of shaders/frame_ubo.glsl is generated from the same
// table, so both sides cannot drift. std140: every member vec4/mat4-sized.
// To change the UBO: edit the table, regenerate the shader block, run
// tests/FrameLanesTest.cpp (see FrameLanes.hpp for the workflow).
struct FrameUniforms {
#define MEADOWS_FRAME_LANE_INIT(...) { __VA_ARGS__ }
#define MEADOWS_FRAME_LANE_MEMBER(cppType, cppName, init, glslType, glslName, \
                                  count, doc)                                 \
    cppType cppName MEADOWS_FRAME_LANE_INIT init;
    MEADOWS_FRAME_LANES(MEADOWS_FRAME_LANE_MEMBER)
#undef MEADOWS_FRAME_LANE_MEMBER
#undef MEADOWS_FRAME_LANE_INIT
};

// --- std140 layout lock -----------------------------------------------------
// The shaders compile against the GLSL block generated from the same table,
// so a table edit moves both sides together; these asserts pin the byte
// offsets independently of the generator, catching a non-vec4-multiple
// member (std140 padding the C++ struct does not have) and a frame_ubo.glsl
// not regenerated after the edit (its block would then disagree with these
// numbers). After a table edit, rewrite the list: tests/FrameLanesTest.cpp
// walks the lanes and checks each offset against the running std140 size.
static_assert(offsetof(FrameUniforms, viewProj) == 0);
static_assert(offsetof(FrameUniforms, invViewProj) == 64);
static_assert(offsetof(FrameUniforms, cameraPos) == 128);
static_assert(offsetof(FrameUniforms, time) == 144);
static_assert(offsetof(FrameUniforms, sunDirection) == 160);
static_assert(offsetof(FrameUniforms, sunColor) == 176);
static_assert(offsetof(FrameUniforms, sunGlowColor) == 192);
static_assert(offsetof(FrameUniforms, ambientColor) == 208);
static_assert(offsetof(FrameUniforms, zenithColor) == 224);
static_assert(offsetof(FrameUniforms, horizonColor) == 240);
static_assert(offsetof(FrameUniforms, horizonFarColor) == 256);
static_assert(offsetof(FrameUniforms, terrainInfo) == 272);
static_assert(offsetof(FrameUniforms, postInfo) == 288);
static_assert(offsetof(FrameUniforms, fogInfo) == 304);
static_assert(offsetof(FrameUniforms, sunViewProj) == 320);
static_assert(offsetof(FrameUniforms, cascadeSplits) == 512);
static_assert(offsetof(FrameUniforms, shadowInfo) == 528);
static_assert(offsetof(FrameUniforms, screenInfo) == 544);
static_assert(offsetof(FrameUniforms, cloudInfo) == 560);
static_assert(offsetof(FrameUniforms, sunScreen) == 576);
static_assert(offsetof(FrameUniforms, cloudMapInfo) == 592);
static_assert(offsetof(FrameUniforms, waterMapInfo) == 608);
static_assert(offsetof(FrameUniforms, windInfo) == 624);
static_assert(offsetof(FrameUniforms, terrainLightInfo) == 640);
static_assert(offsetof(FrameUniforms, submersionInfo) == 656);
static_assert(offsetof(FrameUniforms, stormInfo) == 672);
static_assert(offsetof(FrameUniforms, rainOcclusionViewProj) == 688);
static_assert(offsetof(FrameUniforms, grassBendInfo) == 752);
static_assert(offsetof(FrameUniforms, grassShapeInfo) == 768);
static_assert(offsetof(FrameUniforms, grassLodInfo) == 784);
static_assert(offsetof(FrameUniforms, grassBaseTint) == 800);
static_assert(offsetof(FrameUniforms, grassTipTint) == 816);
static_assert(offsetof(FrameUniforms, giInfo) == 832);
static_assert(offsetof(FrameUniforms, giGridInfo) == 848);
static_assert(offsetof(FrameUniforms, giBandInfo) == 864);
static_assert(offsetof(FrameUniforms, leafLodInfo) == 880);
static_assert(offsetof(FrameUniforms, fogSunInfo) == 896);
static_assert(offsetof(FrameUniforms, stylizedDiffuseInfo) == 912);
static_assert(offsetof(FrameUniforms, stylizedShadowInfo) == 928);
static_assert(offsetof(FrameUniforms, clusterInfo) == 944);
static_assert(offsetof(FrameUniforms, keyShadowAtlasViewProj) == 960);
static_assert(offsetof(FrameUniforms, grassShadeInfo) == 1216);
static_assert(offsetof(FrameUniforms, grassBladeInfo) == 1232);
static_assert(offsetof(FrameUniforms, stylizedSpecInfo) == 1248);
static_assert(offsetof(FrameUniforms, mistInfo) == 1264);
static_assert(offsetof(FrameUniforms, mistShapeInfo) == 1280);
static_assert(offsetof(FrameUniforms, mistMapInfo) == 1296);
static_assert(offsetof(FrameUniforms, mistDetailInfo) == 1312);
static_assert(offsetof(FrameUniforms, mistLightInfo) == 1328);
static_assert(offsetof(FrameUniforms, fogLayerInfo) == 1344);
static_assert(offsetof(FrameUniforms, cloudVolInfo) == 1360);
static_assert(offsetof(FrameUniforms, cloudVolLightInfo) == 1376);
static_assert(offsetof(FrameUniforms, cloudVolShapeInfo) == 1392);
static_assert(offsetof(FrameUniforms, cloudVolRimInfo) == 1408);
static_assert(offsetof(FrameUniforms, mistPuffInfo) == 1424);
static_assert(offsetof(FrameUniforms, waterDebugInfo) == 1440);
static_assert(offsetof(FrameUniforms, seasonInfo) == 1456);
static_assert(offsetof(FrameUniforms, leafSeason) == 1472);
static_assert(offsetof(FrameUniforms, splatDetailInfo) == 1600);
static_assert(offsetof(FrameUniforms, terrainShadeMapInfo) == 1616);
static_assert(offsetof(FrameUniforms, splatVarietyInfo) == 1632);
static_assert(offsetof(FrameUniforms, grassSpeciesShape) == 1648);
static_assert(offsetof(FrameUniforms, grassSpeciesBase) == 1744);
static_assert(offsetof(FrameUniforms, grassSpeciesTip) == 1840);
static_assert(offsetof(FrameUniforms, ssaoInfo) == 1936);
static_assert(offsetof(FrameUniforms, surfSheenInfo) == 1952);
static_assert(offsetof(FrameUniforms, waterSimMapInfo) == 1968);
static_assert(offsetof(FrameUniforms, waterSimTuneInfo) == 1984);
static_assert(offsetof(FrameUniforms, waterSimFrozen) == 2000);
static_assert(offsetof(FrameUniforms, fireScorchInfo) == 2064);
static_assert(offsetof(FrameUniforms, fireEmberInfo) == 2080);
static_assert(offsetof(FrameUniforms, fireEmberHot) == 2096);
static_assert(offsetof(FrameUniforms, fireEmberCold) == 2112);
static_assert(offsetof(FrameUniforms, fireCharInfo) == 2128);
static_assert(offsetof(FrameUniforms, fireFlameInfo) == 2144);
static_assert(offsetof(FrameUniforms, fireFlameLook) == 2160);
static_assert(offsetof(FrameUniforms, windDirInfo) == 2176);
static_assert(offsetof(FrameUniforms, fogTailInfo) == 2192);
static_assert(offsetof(FrameUniforms, vegInfo) == 2208);
static_assert(sizeof(FrameUniforms) == 2224,
              "FrameUniforms changed: regenerate frame_ubo.glsl and rewrite "
              "the offset list above (tests/FrameLanesTest.cpp prints both)");

} // namespace render

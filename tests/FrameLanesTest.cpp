#include <doctest/doctest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "engine/platform/Paths.hpp"
#include "engine/render/landscape/FrameLanes.hpp"
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/render/landscape/GrassSpecies.hpp"
#include "engine/render/landscape/ShadowMapper.hpp"

// The per-frame UBO contract (docs/RENDERING.md §2.3): the C++ struct and
// the GLSL block are generated from ONE table (FrameLanes.hpp), and the
// checked-in shaders/frame_ubo.glsl must be that generator's output. A
// lane added to the table without regenerating the file, or a hand edit of
// the file, fails here instead of desyncing every shader read at runtime.

namespace {

// The shipped file: from the repo when the test runs from a checkout, else
// next to the executable (the game's data/shaders sync).
std::filesystem::path frameUboPath() {
    const auto fromRepo = std::filesystem::path(__FILE__)
                              .parent_path()
                              .parent_path() /
                          "engine" / "render" / "landscape" / "shaders" /
                          "frame_ubo.glsl";
    if (std::filesystem::exists(fromRepo)) {
        return fromRepo;
    }
    return platform::executableDir() / "data" / "shaders" / "frame_ubo.glsl";
}

// Read as bytes; only the line endings are normalised (git's autocrlf may
// check the file out with CRLF).
str readNormalised(const std::filesystem::path& path) {
    std::ifstream in { path, std::ios::binary };
    std::stringstream buffer;
    buffer << in.rdbuf();
    str text = buffer.str();
    std::erase(text, '\r');
    return text;
}

u32 std140Size(const render::FrameLaneInfo& lane) {
    const u32 element = str { lane.glslType } == "mat4" ? 64u : 16u;
    return element * (lane.count > 0 ? lane.count : 1u);
}

} // namespace

TEST_CASE("frame lanes: shaders/frame_ubo.glsl is the generator's output") {
    const auto path = frameUboPath();
    REQUIRE_MESSAGE(std::filesystem::exists(path),
                    "frame_ubo.glsl not found at ", path.string());
    const str shipped = readNormalised(path);
    const str generated = render::frameUboGlsl();
    if (shipped != generated) {
        MESSAGE("shaders/frame_ubo.glsl is stale. Regenerate it by "
                "replacing the file's content with the text below (the "
                "output of render::frameUboGlsl()):\n\n",
                generated);
    }
    CHECK(shipped == generated);
}

TEST_CASE("frame lanes: std140 layout — contiguous vec4/mat4 lanes") {
    const auto lanes = render::frameLanes();
    CHECK(lanes.size() == 79);
    u32 running = 0;
    for (const render::FrameLaneInfo& lane : lanes) {
        INFO("lane ", lane.cppName);
        CHECK(lane.offset == running);
        CHECK(lane.size == std140Size(lane));
        running += lane.size;
    }
    CHECK(running == sizeof(render::FrameUniforms));
    CHECK(sizeof(render::FrameUniforms) == 2224);

    // Spot checks of the pinned offsets, through the descriptors.
    const auto offsetOf = [&](const char* name) -> u32 {
        for (const render::FrameLaneInfo& lane : lanes) {
            if (str { lane.cppName } == name) {
                return lane.offset;
            }
        }
        return ~0u;
    };
    CHECK(offsetOf("viewProj") == 0);
    CHECK(offsetOf("sunViewProj") == 320);
    CHECK(offsetOf("keyShadowAtlasViewProj") == 960);
    CHECK(offsetOf("leafSeason") == 1472);
    CHECK(offsetOf("grassSpeciesShape") == 1648);
    CHECK(offsetOf("fogTailInfo") == 2192);
    CHECK(offsetOf("vegInfo") == 2208);
}

TEST_CASE("frame lanes: GLSL names are the C++ names with a `u` prefix") {
    for (const render::FrameLaneInfo& lane : render::frameLanes()) {
        INFO("lane ", lane.cppName);
        str expected = "u";
        expected += lane.cppName;
        expected[1] = static_cast<char>(std::toupper(expected[1]));
        CHECK(str { lane.glslName } == expected);
    }
}

TEST_CASE("frame lanes: array lengths follow their owning constants") {
    const auto countOf = [](const char* name) -> u32 {
        for (const render::FrameLaneInfo& lane : render::frameLanes()) {
            if (str { lane.cppName } == name) {
                return lane.count;
            }
        }
        return ~0u;
    };
    CHECK(countOf("sunViewProj") == render::ShadowMapper::kCascadeCount);
    CHECK(countOf("grassSpeciesShape") == render::kGrassSpeciesCount);
    CHECK(countOf("grassSpeciesBase") == render::kGrassSpeciesCount);
    CHECK(countOf("grassSpeciesTip") == render::kGrassSpeciesCount);

    // The GLSL side reads the same values from the generated preamble.
    const str generated = render::frameUboGlsl();
    CHECK(generated.find("const int kGrassSpeciesCount = " +
                         std::to_string(render::kGrassSpeciesCount) + ";") !=
          str::npos);
    CHECK(generated.find("layout(std140, binding = 0) uniform FrameUbo {") !=
          str::npos);
}

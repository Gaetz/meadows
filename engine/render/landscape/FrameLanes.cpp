#include "engine/render/landscape/FrameLanes.hpp"

#include <charconv>
#include <cstddef>
#include <string_view>
#include <type_traits>

#include "engine/render/landscape/FarTerrain.hpp"
#include "engine/render/landscape/FrameDecisions.hpp"
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/render/landscape/GrassSpecies.hpp"
#include "engine/render/landscape/LightClusters.hpp"
#include "engine/render/landscape/MistMap.hpp"
#include "engine/render/landscape/RadianceCascades.hpp"
#include "engine/render/landscape/WaterSystem.hpp"

namespace render {

namespace {

// The array lanes' lengths are literals in the lane table (the X-macro
// needs one token); these pin them to the constants they stand for.
static_assert(sizeof(FrameUniforms::grassSpeciesShape) ==
              kGrassSpeciesCount * sizeof(Vec4));
static_assert(sizeof(FrameUniforms::grassSpeciesBase) ==
              kGrassSpeciesCount * sizeof(Vec4));
static_assert(sizeof(FrameUniforms::grassSpeciesTip) ==
              kGrassSpeciesCount * sizeof(Vec4));

#define MEADOWS_FRAME_LANE_INFO(cppType, cppName, init, glslType, glslName, \
                                count, doc)                                 \
    FrameLaneInfo { #cppName, #glslType, #glslName, count,                  \
                    static_cast<u32>(offsetof(FrameUniforms, cppName)),     \
                    static_cast<u32>(sizeof(FrameUniforms::cppName)), doc },
constexpr FrameLaneInfo kLanes[] = { MEADOWS_FRAME_LANES(
    MEADOWS_FRAME_LANE_INFO) };
#undef MEADOWS_FRAME_LANE_INFO

template <class T> struct GlslScalar;
template <> struct GlslScalar<i32> {
    static constexpr std::string_view name = "int";
};
template <> struct GlslScalar<f32> {
    static constexpr std::string_view name = "float";
};

// Shortest round-trip decimal; floats always carry a '.' so GLSL reads
// them as float literals (5200 -> "5200.0").
template <class T> str glslLiteral(T value) {
    char buffer[64];
    const auto res =
        std::to_chars(buffer, buffer + sizeof(buffer), value);
    str text(buffer, res.ptr);
    if constexpr (std::is_floating_point_v<T>) {
        if (text.find_first_of(".eE") == str::npos) {
            text += ".0";
        }
    }
    return text;
}

void appendComment(str& out, std::string_view doc) {
    while (!doc.empty()) {
        const size_t nl = doc.find('\n');
        const std::string_view line = doc.substr(0, nl);
        out += "    // ";
        out += line;
        out += '\n';
        if (nl == std::string_view::npos) {
            break;
        }
        doc.remove_prefix(nl + 1);
    }
}

// Top-level (unindented) comment lines.
void appendPreambleComment(str& out, std::string_view doc) {
    while (!doc.empty()) {
        const size_t nl = doc.find('\n');
        out += "// ";
        out += doc.substr(0, nl);
        out += '\n';
        if (nl == std::string_view::npos) {
            break;
        }
        doc.remove_prefix(nl + 1);
    }
}

template <class T, class Expr>
void appendConstant(str& out, std::string_view name, Expr value,
                    std::string_view doc) {
    // An int constant must come from an integral C++ value and a float one
    // from a floating value — the table's type column cannot lie.
    static_assert(std::is_integral_v<Expr> == std::is_integral_v<T>,
                  "frame constant type column disagrees with its C++ value");
    appendPreambleComment(out, doc);
    out += "const ";
    out += GlslScalar<T>::name;
    out += ' ';
    out += name;
    out += " = ";
    out += glslLiteral(static_cast<T>(value));
    out += ";\n";
}

} // namespace

std::span<const FrameLaneInfo> frameLanes() {
    return kLanes;
}

str frameUboGlsl() {
    str out;
    out += "// GENERATED from engine/render/landscape/FrameLanes.hpp by\n"
           "// render::frameUboGlsl() - do not edit by hand. "
           "tests/FrameLanesTest.cpp\n"
           "// pins this file to the generator and prints the regenerated "
           "text on a\n"
           "// mismatch: paste it here.\n"
           "#ifndef MEADOWS_FRAME_UBO_GLSL\n"
           "#define MEADOWS_FRAME_UBO_GLSL\n"
           "\n"
           "// Constants shared with the C++ side (MEADOWS_FRAME_CONSTANTS); "
           "each one\n"
           "// is read from the declaration that owns it.\n";
#define MEADOWS_FRAME_CONSTANT(cppType, glslName, expr, doc) \
    appendConstant<cppType>(out, #glslName, (expr), doc);
    MEADOWS_FRAME_CONSTANTS(MEADOWS_FRAME_CONSTANT)
#undef MEADOWS_FRAME_CONSTANT
    out += "\n"
           "// Per-frame constants shared by every landscape shader "
           "(MEADOWS_FRAME_LANES\n"
           "// - render::FrameUniforms, uploaded once per frame). std140: "
           "every lane\n"
           "// vec4- or mat4-sized. Edit the table, never this block.\n"
           "layout(std140, binding = 0) uniform FrameUbo {\n";
    for (const FrameLaneInfo& lane : kLanes) {
        appendComment(out, lane.doc);
        out += "    ";
        out += lane.glslType;
        out += ' ';
        out += lane.glslName;
        if (lane.count > 0) {
            out += '[';
            out += glslLiteral(static_cast<i32>(lane.count));
            out += ']';
        }
        out += ";\n";
    }
    out += "};\n"
           "\n"
           "#endif\n";
    return out;
}

} // namespace render

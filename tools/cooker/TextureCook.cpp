#include "TextureCook.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <optional>

#include <toml++/toml.hpp>

#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <bc7enc.h>
#include <rgbcx.h>

#include "engine/assets/CookedTexture.hpp"
#include "engine/core/Log.hpp"

namespace cooker {

namespace {

namespace fs = std::filesystem;

constexpr u32 kSize = 512; // material tile resolution (brief §1: never above)
constexpr u32 kMips = 10;  // 512 -> 1

struct Rgba {
    u32 width { 0 };
    u32 height { 0 };
    vector<u8> pixels; // width * height * 4
};

struct Gray16 {
    u32 width { 0 };
    u32 height { 0 };
    vector<u16> pixels;
};

// One material's source set. Missing optional maps get neutral defaults so
// every layer exists in every array (the splat shader indexes them all).
struct MaterialSources {
    str name;
    fs::path albedo; // required
    fs::path normal;
    fs::path orm;
    fs::path ao;
    fs::path roughness;
    fs::path metallic;
    fs::path height;
    bool flipNormalY { false };
    // Normalize this material's mean albedo to another layer's mean
    // (per channel): family variants stay in one color family — their
    // variation lives in content and relief, not in color patches.
    // `harmonizeWith` picks the anchor layer (0 = grass by default).
    bool harmonize { false };
    i64 harmonizeWith { 0 };
};

std::optional<Rgba> loadRgba(const fs::path& path) {
    int w = 0;
    int h = 0;
    int comp = 0;
    u8* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (data == nullptr) {
        LOG_ERROR("cook-terrain-materials: cannot decode '{}': {}",
                  path.string(), stbi_failure_reason());
        return std::nullopt;
    }
    Rgba img { static_cast<u32>(w), static_cast<u32>(h), {} };
    img.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);
    return img;
}

std::optional<Gray16> loadGray16(const fs::path& path) {
    int w = 0;
    int h = 0;
    int comp = 0;
    u16* data = stbi_load_16(path.string().c_str(), &w, &h, &comp, 1);
    if (data == nullptr) {
        LOG_ERROR("cook-terrain-materials: cannot decode '{}': {}",
                  path.string(), stbi_failure_reason());
        return std::nullopt;
    }
    Gray16 img { static_cast<u32>(w), static_cast<u32>(h), {} };
    img.pixels.assign(data, data + static_cast<size_t>(w) * h);
    stbi_image_free(data);
    return img;
}

Rgba resizeRgba(const Rgba& src, u32 size, bool srgb) {
    if (src.width == size && src.height == size) {
        return src;
    }
    Rgba dst { size, size, {} };
    dst.pixels.resize(static_cast<size_t>(size) * size * 4);
    stbir_resize(src.pixels.data(), static_cast<int>(src.width),
                 static_cast<int>(src.height), 0, dst.pixels.data(),
                 static_cast<int>(size), static_cast<int>(size), 0,
                 STBIR_4CHANNEL,
                 srgb ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8,
                 STBIR_EDGE_WRAP, STBIR_FILTER_MITCHELL);
    return dst;
}

Gray16 resizeGray16(const Gray16& src, u32 size) {
    if (src.width == size && src.height == size) {
        return src;
    }
    Gray16 dst { size, size, {} };
    dst.pixels.resize(static_cast<size_t>(size) * size);
    stbir_resize(src.pixels.data(), static_cast<int>(src.width),
                 static_cast<int>(src.height), 0, dst.pixels.data(),
                 static_cast<int>(size), static_cast<int>(size), 0,
                 STBIR_1CHANNEL, STBIR_TYPE_UINT16, STBIR_EDGE_WRAP,
                 STBIR_FILTER_MITCHELL);
    return dst;
}

// 4x4 block fetch with edge clamp, so mips below 4 px still encode.
void fetchBlock(const Rgba& img, u32 bx, u32 by, u8 out[64]) {
    for (u32 y = 0; y < 4; ++y) {
        const u32 sy = std::min(by * 4 + y, img.height - 1);
        for (u32 x = 0; x < 4; ++x) {
            const u32 sx = std::min(bx * 4 + x, img.width - 1);
            const u8* px = &img.pixels[(static_cast<size_t>(sy) * img.width +
                                        sx) * 4];
            std::copy(px, px + 4, &out[(y * 4 + x) * 4]);
        }
    }
}

vector<u8> encodeBc7(const Rgba& img,
                     const bc7enc_compress_block_params& params) {
    const u32 bw = (img.width + 3) / 4;
    const u32 bh = (img.height + 3) / 4;
    vector<u8> blocks(static_cast<size_t>(bw) * bh * 16);
    u8 pixels[64];
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            fetchBlock(img, bx, by, pixels);
            bc7enc_compress_block(
                &blocks[(static_cast<size_t>(by) * bw + bx) * 16], pixels,
                &params);
        }
    }
    return blocks;
}

vector<u8> encodeBc5(const Rgba& img) {
    const u32 bw = (img.width + 3) / 4;
    const u32 bh = (img.height + 3) / 4;
    vector<u8> blocks(static_cast<size_t>(bw) * bh * 16);
    u8 pixels[64];
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            fetchBlock(img, bx, by, pixels);
            rgbcx::encode_bc5(&blocks[(static_cast<size_t>(by) * bw + bx) *
                                      16],
                              pixels, 0, 1, 4);
        }
    }
    return blocks;
}

Rgba neutralRgba(u8 r, u8 g, u8 b, u8 a) {
    Rgba img { kSize, kSize, {} };
    img.pixels.resize(static_cast<size_t>(kSize) * kSize * 4);
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i + 0] = r;
        img.pixels[i + 1] = g;
        img.pixels[i + 2] = b;
        img.pixels[i + 3] = a;
    }
    return img;
}

Gray16 neutralGray16(u16 value) {
    Gray16 img { kSize, kSize, {} };
    img.pixels.assign(static_cast<size_t>(kSize) * kSize, value);
    return img;
}

// Every mip resampled from the 512 base (not successively) to avoid
// accumulating filter blur down the chain.
template <typename ImageT, typename ResizeFn>
vector<ImageT> mipChain(const ImageT& base, ResizeFn resize) {
    vector<ImageT> mips;
    mips.reserve(kMips);
    mips.push_back(base);
    for (u32 m = 1; m < kMips; ++m) {
        mips.push_back(resize(base, kSize >> m));
    }
    return mips;
}

std::optional<vector<MaterialSources>> parseManifest(const fs::path& path) {
    toml::parse_result parsed = toml::parse_file(path.string());
    if (!parsed) {
        LOG_ERROR("cook-terrain-materials: manifest parse error: {}",
                  str(parsed.error().description()));
        return std::nullopt;
    }
    const fs::path baseDir = path.parent_path();
    const toml::table& root = parsed.table();
    const toml::array* materials = root["material"].as_array();
    if (materials == nullptr || materials->empty()) {
        LOG_ERROR("cook-terrain-materials: manifest has no [[material]]");
        return std::nullopt;
    }
    vector<MaterialSources> out;
    for (const toml::node& node : *materials) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            continue;
        }
        MaterialSources mat;
        mat.name = (*table)["name"].value_or(str {});
        const auto source = [&](const char* key) -> fs::path {
            const str rel = (*table)[key].value_or(str {});
            return rel.empty() ? fs::path {} : baseDir / rel;
        };
        mat.albedo = source("albedo");
        mat.normal = source("normal");
        mat.orm = source("orm");
        mat.ao = source("ao");
        mat.roughness = source("roughness");
        mat.metallic = source("metallic");
        mat.height = source("height");
        mat.flipNormalY = (*table)["flip_normal_y"].value_or(false);
        mat.harmonize = (*table)["harmonize"].value_or(false);
        mat.harmonizeWith =
            (*table)["harmonizeWith"].value_or<i64>(0);
        if (mat.name.empty() || mat.albedo.empty()) {
            LOG_ERROR("cook-terrain-materials: every [[material]] needs "
                      "'name' and 'albedo'");
            return std::nullopt;
        }
        out.push_back(std::move(mat));
    }
    return out;
}

// Loads one channel of a packed-or-separate set as a grayscale-in-RGBA
// helper: returns the channel `c` of `img` at index i.
u8 channel(const Rgba& img, size_t i, u32 c) {
    return img.pixels[i * 4 + c];
}

std::optional<Rgba> buildOrm(const MaterialSources& mat) {
    if (!mat.orm.empty()) {
        auto img = loadRgba(mat.orm);
        if (!img) {
            return std::nullopt;
        }
        return resizeRgba(*img, kSize, false);
    }
    // Pack AO/Roughness/Metallic from separate maps; missing ones default
    // to neutral (full AO, full roughness, no metal).
    Rgba out = neutralRgba(255, 255, 0, 255);
    const auto packFrom = [&](const fs::path& path, u32 dstChannel) -> bool {
        if (path.empty()) {
            return true;
        }
        auto img = loadRgba(path);
        if (!img) {
            return false;
        }
        const Rgba resized = resizeRgba(*img, kSize, false);
        for (size_t i = 0; i < static_cast<size_t>(kSize) * kSize; ++i) {
            out.pixels[i * 4 + dstChannel] = channel(resized, i, 0);
        }
        return true;
    };
    if (!packFrom(mat.ao, 0) || !packFrom(mat.roughness, 1) ||
        !packFrom(mat.metallic, 2)) {
        return std::nullopt;
    }
    return out;
}

} // namespace

int cookTerrainMaterials(const char* manifestPath, const char* outDir) {
    const auto materials = parseManifest(manifestPath);
    if (!materials) {
        return 1;
    }
    const fs::path out { outDir };
    std::error_code ec;
    fs::create_directories(out, ec);

    bc7enc_compress_block_init();
    rgbcx::init();
    bc7enc_compress_block_params srgbParams;
    bc7enc_compress_block_params_init(&srgbParams); // perceptual (albedo)
    bc7enc_compress_block_params linearParams;
    bc7enc_compress_block_params_init(&linearParams);
    bc7enc_compress_block_params_init_linear_weights(&linearParams); // ORM

    const u32 layers = static_cast<u32>(materials->size());
    // Per-layer average albedo (display bytes) — CPU consumers can't
    // decode the BC payload (blade root albedo, docs/GRASS-REDO.md).
    vector<u32> albedoAverages(layers, 0xff808080u);
    // [mip][layer] payload slices, assembled mip-major at the end (the
    // CookedTexture/createTexture layout contract).
    vector<vector<vector<u8>>> albedoMips(kMips, vector<vector<u8>>(layers));
    vector<vector<vector<u8>>> normalMips(kMips, vector<vector<u8>>(layers));
    vector<vector<vector<u8>>> ormMips(kMips, vector<vector<u8>>(layers));
    vector<vector<vector<u8>>> heightMips(kMips, vector<vector<u8>>(layers));

    // Anchor means, per layer already cooked this run

    // (harmonizeWith indexes into them).

    vector<array<f64, 3>> layerMeans;

    for (u32 layer = 0; layer < layers; ++layer) {
        const MaterialSources& mat = (*materials)[layer];
        LOG_INFO("cook-terrain-materials: [{}] {}", layer, mat.name);

        auto albedo = loadRgba(mat.albedo);
        if (!albedo) {
            return 1;
        }
        Rgba albedoBase = resizeRgba(*albedo, kSize, true);
        {
            f64 mean[3] = { 0, 0, 0 };
            for (size_t i = 0; i < albedoBase.pixels.size(); i += 4) {
                mean[0] += albedoBase.pixels[i];
                mean[1] += albedoBase.pixels[i + 1];
                mean[2] += albedoBase.pixels[i + 2];
            }
            const f64 count = albedoBase.pixels.size() / 4.0;
            for (f64& m : mean) {
                m /= count;
            }
            if (mat.harmonize &&
                mat.harmonizeWith >= 0 &&
                static_cast<size_t>(mat.harmonizeWith) <
                    layerMeans.size()) {
                const array<f64, 3>& anchor =
                    layerMeans[static_cast<size_t>(mat.harmonizeWith)];
                const f64 scale[3] = { anchor[0] / std::max(mean[0], 1.0),
                                       anchor[1] / std::max(mean[1], 1.0),
                                       anchor[2] / std::max(mean[2], 1.0) };
                for (size_t i = 0; i < albedoBase.pixels.size(); i += 4) {
                    for (u32 c = 0; c < 3; ++c) {
                        albedoBase.pixels[i + c] = static_cast<u8>(
                            std::clamp(albedoBase.pixels[i + c] * scale[c],
                                       0.0, 255.0));
                    }
                }
            }
            // Every layer's (pre-harmonize) mean joins the anchor table
            // in order — later variants index it by harmonizeWith.
            layerMeans.push_back({ mean[0], mean[1], mean[2] });
        }
        const auto albedoChain =
            mipChain(albedoBase, [](const Rgba& base, u32 size) {
                return resizeRgba(base, size, true);
            });
        {
            u64 sums[3] = { 0, 0, 0 };
            const vector<u8>& px = albedoChain[0].pixels;
            for (size_t i = 0; i < px.size(); i += 4) {
                sums[0] += px[i];
                sums[1] += px[i + 1];
                sums[2] += px[i + 2];
            }
            const u64 count = px.size() / 4;
            albedoAverages[layer] =
                static_cast<u32>(sums[0] / count) |
                (static_cast<u32>(sums[1] / count) << 8) |
                (static_cast<u32>(sums[2] / count) << 16) | 0xff000000u;
        }

        Rgba normalBase = neutralRgba(128, 128, 255, 255);
        if (!mat.normal.empty()) {
            auto normal = loadRgba(mat.normal);
            if (!normal) {
                return 1;
            }
            normalBase = resizeRgba(*normal, kSize, false);
            if (mat.flipNormalY) {
                for (size_t i = 1; i < normalBase.pixels.size(); i += 4) {
                    normalBase.pixels[i] =
                        static_cast<u8>(255 - normalBase.pixels[i]);
                }
            }
        }
        const auto normalChain =
            mipChain(normalBase, [](const Rgba& base, u32 size) {
                return resizeRgba(base, size, false);
            });

        auto orm = buildOrm(mat);
        if (!orm) {
            return 1;
        }
        const auto ormChain =
            mipChain(*orm, [](const Rgba& base, u32 size) {
                return resizeRgba(base, size, false);
            });

        Gray16 heightBase = neutralGray16(32768);
        if (!mat.height.empty()) {
            auto height = loadGray16(mat.height);
            if (!height) {
                return 1;
            }
            heightBase = resizeGray16(*height, kSize);
        }
        const auto heightChain =
            mipChain(heightBase, [](const Gray16& base, u32 size) {
                return resizeGray16(base, size);
            });

        for (u32 m = 0; m < kMips; ++m) {
            albedoMips[m][layer] = encodeBc7(albedoChain[m], srgbParams);
            normalMips[m][layer] = encodeBc5(normalChain[m]);
            ormMips[m][layer] = encodeBc7(ormChain[m], linearParams);
            const auto& height = heightChain[m];
            auto& dst = heightMips[m][layer];
            dst.resize(height.pixels.size() * 2);
            std::memcpy(dst.data(), height.pixels.data(), dst.size());
        }
    }

    const auto assemble = [&](const vector<vector<vector<u8>>>& mips,
                              rhi::TextureFormat format,
                              const char* filename,
                              const vector<u32>* averages = nullptr) -> bool {
        assets::CookedTexture tex { .width = kSize,
                                    .height = kSize,
                                    .arrayLayers = layers,
                                    .mipLevels = kMips,
                                    .format = format };
        if (averages != nullptr) {
            tex.layerAverages = *averages;
        }
        for (u32 m = 0; m < kMips; ++m) {
            for (u32 layer = 0; layer < layers; ++layer) {
                const vector<u8>& slice = mips[m][layer];
                tex.payload.insert(tex.payload.end(), slice.begin(),
                                   slice.end());
            }
        }
        const fs::path path = out / filename;
        if (!assets::saveCookedTexture(path, tex)) {
            return false;
        }
        LOG_INFO("cook-terrain-materials: wrote {} ({} layers, {} bytes)",
                 path.string(), layers, tex.payload.size());
        return true;
    };

    if (!assemble(albedoMips, rhi::TextureFormat::BC7_SRGB,
                  "terrain_albedo.mtex", &albedoAverages) ||
        !assemble(normalMips, rhi::TextureFormat::BC5_UNORM,
                  "terrain_normal.mtex") ||
        !assemble(ormMips, rhi::TextureFormat::BC7_UNORM,
                  "terrain_orm.mtex") ||
        !assemble(heightMips, rhi::TextureFormat::R16_UNORM,
                  "terrain_height.mtex")) {
        return 1;
    }
    return 0;
}

} // namespace cooker

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "engine/assets/CookedTexture.hpp"
#include "engine/rhi/Rhi.hpp"

// The upload-size arithmetic is the contract shared by the Vulkan backend,
// the .mtex loader and the cooker — a drift corrupts uploads silently.
TEST_CASE("rhi format block helpers") {
    using rhi::TextureFormat;

    CHECK(rhi::isBlockCompressed(TextureFormat::BC7_SRGB));
    CHECK(rhi::isBlockCompressed(TextureFormat::BC5_UNORM));
    CHECK_FALSE(rhi::isBlockCompressed(TextureFormat::R16_UNORM));
    CHECK_FALSE(rhi::isBlockCompressed(TextureFormat::RGBA8));

    // BC: 16 bytes per 4x4 block, rounded up — mips below 4 px still take
    // one block.
    CHECK(rhi::mipLevelBytes(TextureFormat::BC7_SRGB, 512, 512) ==
          128u * 128u * 16u);
    CHECK(rhi::mipLevelBytes(TextureFormat::BC7_SRGB, 4, 4) == 16u);
    CHECK(rhi::mipLevelBytes(TextureFormat::BC7_SRGB, 2, 2) == 16u);
    CHECK(rhi::mipLevelBytes(TextureFormat::BC5_UNORM, 1, 1) == 16u);
    CHECK(rhi::mipLevelBytes(TextureFormat::R16_UNORM, 512, 512) ==
          512u * 512u * 2u);
    CHECK(rhi::mipLevelBytes(TextureFormat::RGBA8, 3, 3) == 9u * 4u);

    // Full 512 chain, one layer: 10 mips of BC7.
    const rhi::TextureDesc bc7 { .width = 512,
                                 .height = 512,
                                 .mipLevels = 10,
                                 .pixelsIncludeMips = true,
                                 .format = TextureFormat::BC7_SRGB };
    u64 expected = 0;
    for (u32 size = 512; size >= 1; size /= 2) {
        expected += u64((size + 3) / 4) * ((size + 3) / 4) * 16;
    }
    CHECK(rhi::textureDataBytes(bc7) == expected);

    // Layers multiply every mip; without pixelsIncludeMips only the base
    // counts.
    rhi::TextureDesc arr = bc7;
    arr.arrayLayers = 5;
    CHECK(rhi::textureDataBytes(arr) == expected * 5);
    arr.pixelsIncludeMips = false;
    CHECK(rhi::textureDataBytes(arr) == 128u * 128u * 16u * 5u);
}

TEST_CASE("cooked texture round-trip") {
    const auto dir = std::filesystem::temp_directory_path() / "meadows-mtex";
    std::filesystem::create_directories(dir);
    const auto path = dir / "roundtrip.mtex";

    assets::CookedTexture tex { .width = 8,
                                .height = 8,
                                .arrayLayers = 2,
                                .mipLevels = 4,
                                .format = rhi::TextureFormat::R16_UNORM };
    tex.payload.resize(rhi::textureDataBytes(tex.desc()));
    for (size_t i = 0; i < tex.payload.size(); ++i) {
        tex.payload[i] = static_cast<u8>(i * 31u);
    }
    REQUIRE(assets::saveCookedTexture(path, tex));

    const auto loaded = assets::loadCookedTexture(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->width == tex.width);
    CHECK(loaded->height == tex.height);
    CHECK(loaded->arrayLayers == tex.arrayLayers);
    CHECK(loaded->mipLevels == tex.mipLevels);
    CHECK(loaded->format == tex.format);
    CHECK(loaded->payload == tex.payload);

    // The descriptor carries the offline-mips contract for createTexture.
    const rhi::TextureDesc desc = loaded->desc();
    CHECK(desc.pixelsIncludeMips);
    CHECK(desc.wrap == rhi::AddressMode::Repeat);
    CHECK(rhi::textureDataBytes(desc) == loaded->payload.size());
}

TEST_CASE("cooked texture rejects corrupt files") {
    const auto dir = std::filesystem::temp_directory_path() / "meadows-mtex";
    std::filesystem::create_directories(dir);

    // Payload size must match the header's layout exactly.
    assets::CookedTexture bad { .width = 8,
                                .height = 8,
                                .format = rhi::TextureFormat::BC7_SRGB };
    bad.payload.resize(7); // BC 8x8 single mip = 4 blocks = 64 bytes
    CHECK_FALSE(assets::saveCookedTexture(dir / "bad.mtex", bad));

    // Not a .mtex file.
    {
        std::ofstream file(dir / "junk.mtex", std::ios::binary);
        file << "not a texture";
    }
    CHECK_FALSE(assets::loadCookedTexture(dir / "junk.mtex").has_value());

    // Truncated payload.
    assets::CookedTexture ok { .width = 8,
                               .height = 8,
                               .format = rhi::TextureFormat::R16_UNORM };
    ok.payload.resize(rhi::textureDataBytes(ok.desc()), 0x42);
    REQUIRE(assets::saveCookedTexture(dir / "trunc.mtex", ok));
    std::filesystem::resize_file(dir / "trunc.mtex", 40);
    CHECK_FALSE(assets::loadCookedTexture(dir / "trunc.mtex").has_value());

    CHECK_FALSE(assets::loadCookedTexture(dir / "missing.mtex").has_value());
}

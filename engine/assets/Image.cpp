#include "engine/assets/Image.hpp"

#include <stb_image.h>

#include "engine/core/Log.hpp"

namespace assets {

std::optional<Image> loadImageFile(const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(path.string().c_str(), &width, &height,
                                 &channels, STBI_rgb_alpha);
    if (!decoded) {
        LOG_ERROR("Image load failed '{}': {}", path.string(),
                  stbi_failure_reason());
        return std::nullopt;
    }

    Image image;
    image.width = static_cast<u32>(width);
    image.height = static_cast<u32>(height);
    image.pixels.assign(decoded,
                        decoded + static_cast<size_t>(width) * height * 4);
    stbi_image_free(decoded);
    return image;
}

} // namespace assets

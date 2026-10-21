// ps5-homebrew-ui - Load a cover image from a URL into an OpenGL texture.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/cover_loader.hpp"
#include "core/api_client.hpp"
#include "gfx/gl_batch.hpp"

// stb_image implementation must live in exactly one translation unit.
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"

#include <cstring>
#include <vector>

namespace hui::core
{

std::uint32_t load_cover(ApiClient &api, gfx::GlBatch &batch, const std::string &url,
                         std::string &error)
{
    error.clear();

    std::vector<unsigned char> bytes;
    if (!api.fetch_bytes(url, bytes, error))
        return 0;
    if (bytes.empty())
    {
        error = "empty image response";
        return 0;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    // Force 4 channels (RGBA) so the upload matches GL_RGBA8.
    unsigned char *pixels =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                              &channels, 4);
    if (pixels == nullptr)
    {
        error = std::string("stbi_load_from_memory failed: ") + stbi_failure_reason();
        return 0;
    }

    // stb_image returns rows top-to-bottom; OpenGL expects bottom-to-top.
    // Flip the rows so the texture is upright.
    const int row_bytes = width * 4; // RGBA
    std::vector<unsigned char> flipped(static_cast<std::size_t>(row_bytes) * height);
    for (int y = 0; y < height; ++y)
    {
        const unsigned char *src = pixels + static_cast<std::size_t>(y) * row_bytes;
        unsigned char *dst = flipped.data() +
                             static_cast<std::size_t>(height - 1 - y) * row_bytes;
        std::memcpy(dst, src, row_bytes);
    }

    const std::uint32_t texture = batch.create_texture(width, height, flipped.data());
    stbi_image_free(pixels);

    if (texture == 0)
    {
        error = "create_texture returned 0";
        return 0;
    }
    return texture;
}

} // namespace hui::core
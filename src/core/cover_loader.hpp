// ps5-homebrew-ui - Load a cover image from a URL into an OpenGL texture.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace hui::gfx
{
class GlBatch;
}

namespace hui::core
{

class ApiClient;

// Downloads `url`, decodes it as an image (JPEG/PNG), uploads the RGBA
// pixels to the batch, and returns the new texture id (0 on failure).
// `error` receives a human-readable reason on failure.
std::uint32_t load_cover(ApiClient &api, gfx::GlBatch &batch, const std::string &url,
                         std::string &error);

} // namespace hui::core
// ps5-homebrew-ui - Factories of every UI design in the app.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/concept.hpp"

#include <memory>

namespace hui::concepts
{
std::unique_ptr<app::Concept> make_paper(app::Context &context);
} // namespace hui::concepts

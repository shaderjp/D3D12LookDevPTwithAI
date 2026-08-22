#pragma once

#include "TextureLoader.h"

#include <cstdint>
#include <string>

namespace Bistro
{
bool LoadKtx2Texture(
    const std::wstring& path,
    bool srgb,
    std::uint32_t maxDimension,
    TextureData& texture,
    std::string& diagnostics);
}

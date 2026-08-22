#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Bistro
{
struct ExrImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgba;
};

bool LoadExrRgba32Float(const std::wstring& path, ExrImage& image, std::string& diagnostics);
}

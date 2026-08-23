#include "TinyExrLoader.h"

#include <exr.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>

// TinyEXR v3 is C11. Its HTJ2K SIMD translation unit uses the GCC spelling
// for count-leading-zero; MSVC otherwise treats it as an external function.
extern "C" int __builtin_clz(unsigned int value) noexcept
{
    unsigned long mostSignificantBit = 0;
    if (value == 0 || !_BitScanReverse(&mostSignificantBit, value)) return 32;
    return 31 - static_cast<int>(mostSignificantBit);
}
#endif

namespace
{
std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

int FindChannel(const exr_part& part, const char* name)
{
    for (int index = 0; index < part.header.num_channels; ++index)
    {
        if (std::strcmp(part.header.channels[index].name, name) == 0) return index;
    }
    return -1;
}

float ChannelValue(const exr_part& part, int channel, std::size_t index)
{
    if (channel < 0 || !part.images || !part.images[channel]) return 0.0f;
    switch (part.header.channels[channel].pixel_type)
    {
    case EXR_PIXEL_HALF:
    {
        float value = 0.0f;
        exr_half_to_float(static_cast<const std::uint16_t*>(part.images[channel]) + index, &value, 1);
        return value;
    }
    case EXR_PIXEL_FLOAT:
        return static_cast<const float*>(part.images[channel])[index];
    case EXR_PIXEL_UINT:
        return static_cast<float>(static_cast<const std::uint32_t*>(part.images[channel])[index]);
    default:
        return 0.0f;
    }
}
}

namespace Bistro
{
bool LoadExrRgba32Float(const std::wstring& path, ExrImage& image, std::string& diagnostics)
{
    image = {};
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file)
    {
        diagnostics = "EXR file was not found.";
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0 || static_cast<std::uint64_t>(size) > (std::numeric_limits<std::size_t>::max)())
    {
        diagnostics = "EXR file size is invalid.";
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file)
    {
        diagnostics = "EXR file could not be read.";
        return false;
    }

    exr_image source{};
    const exr_result loadResult = exr_load_from_memory(bytes.data(), bytes.size(), nullptr, &source);
    if (!EXR_OK(loadResult))
    {
        diagnostics = std::string("TinyEXR failed to decode '") + WideToUtf8(path) + "': " + exr_result_string(loadResult);
        return false;
    }
    struct ImageGuard
    {
        exr_image* image;
        ~ImageGuard() { exr_image_free(image); }
    } guard{ &source };

    if (source.num_parts < 1 || !source.parts || source.parts[0].is_deep || !source.parts[0].images)
    {
        diagnostics = "EXR has no supported flat image part.";
        return false;
    }
    const exr_part& part = source.parts[0];
    if (part.width <= 0 || part.height <= 0 ||
        static_cast<std::uint64_t>(part.width) * static_cast<std::uint64_t>(part.height) >
            (std::numeric_limits<std::size_t>::max)() / (4u * sizeof(float)))
    {
        diagnostics = "EXR dimensions exceed addressable memory.";
        return false;
    }

    image.width = static_cast<std::uint32_t>(part.width);
    image.height = static_cast<std::uint32_t>(part.height);
    const std::size_t pixelCount = static_cast<std::size_t>(part.width) * static_cast<std::size_t>(part.height);
    image.rgba.resize(pixelCount * 4u);
    if (exr_part_is_luminance_chroma(&part))
    {
        float* rgba = nullptr;
        int width = 0;
        int height = 0;
        const exr_result result = exr_part_yc_to_rgba_float(nullptr, &part, &rgba, &width, &height);
        if (!EXR_OK(result) || !rgba || width != part.width || height != part.height)
        {
            std::free(rgba);
            diagnostics = "TinyEXR could not reconstruct luminance-chroma pixels.";
            return false;
        }
        std::copy_n(rgba, image.rgba.size(), image.rgba.begin());
        std::free(rgba);
    }
    else
    {
        const int red = FindChannel(part, "R");
        const int green = FindChannel(part, "G");
        const int blue = FindChannel(part, "B");
        const int alpha = FindChannel(part, "A");
        const int luminance = FindChannel(part, "Y");
        if ((red < 0 || green < 0 || blue < 0) && luminance < 0)
        {
            diagnostics = "EXR first part has neither RGB nor Y channels.";
            return false;
        }
        for (std::size_t index = 0; index < pixelCount; ++index)
        {
            const float y = luminance >= 0 ? ChannelValue(part, luminance, index) : 0.0f;
            image.rgba[index * 4u] = red >= 0 ? ChannelValue(part, red, index) : y;
            image.rgba[index * 4u + 1u] = green >= 0 ? ChannelValue(part, green, index) : y;
            image.rgba[index * 4u + 2u] = blue >= 0 ? ChannelValue(part, blue, index) : y;
            image.rgba[index * 4u + 3u] = alpha >= 0 ? ChannelValue(part, alpha, index) : 1.0f;
        }
    }

    for (std::size_t index = 0; index < pixelCount; ++index)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            float& value = image.rgba[index * 4u + channel];
            if (!std::isfinite(value) || value < 0.0f) value = 0.0f;
        }
        float& alpha = image.rgba[index * 4u + 3u];
        if (!std::isfinite(alpha)) alpha = 1.0f;
    }
    diagnostics.clear();
    return true;
}
}

#include "TinyExrLoader.h"

#include <exr.h>
#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void InitializePart(exr_part& part, exr_channel* channels, void** images, int channelCount)
{
    part = {};
    part.width = 2;
    part.height = 1;
    part.header.part_type = EXR_PART_SCANLINE;
    part.header.compression = EXR_COMPRESSION_NONE;
    part.header.line_order = EXR_LINEORDER_INCREASING_Y;
    part.header.data_window = { 0, 0, 1, 0 };
    part.header.display_window = part.header.data_window;
    part.header.pixel_aspect_ratio = 1.0f;
    part.header.screen_window_width = 1.0f;
    part.header.num_channels = channelCount;
    part.header.channels = channels;
    part.images = images;
}

void InitializeChannel(exr_channel& channel, const char* name, exr_pixel_type type)
{
    channel = {};
    strcpy_s(channel.name, name);
    channel.pixel_type = type;
    channel.x_sampling = 1;
    channel.y_sampling = 1;
}

void SaveFloatRgba(const std::filesystem::path& path)
{
    exr_channel channels[4];
    // OpenEXR channel lists are lexicographically ordered.
    InitializeChannel(channels[0], "A", EXR_PIXEL_FLOAT);
    InitializeChannel(channels[1], "B", EXR_PIXEL_FLOAT);
    InitializeChannel(channels[2], "G", EXR_PIXEL_FLOAT);
    InitializeChannel(channels[3], "R", EXR_PIXEL_FLOAT);
    float a[2] = { 0.25f, std::numeric_limits<float>::quiet_NaN() };
    float b[2] = { 0.3f, -std::numeric_limits<float>::infinity() };
    float g[2] = { 0.2f, std::numeric_limits<float>::infinity() };
    float r[2] = { 0.1f, std::numeric_limits<float>::quiet_NaN() };
    void* images[4] = { a, b, g, r };
    exr_part part;
    InitializePart(part, channels, images, 4);
    exr_image image{};
    image.num_parts = 1;
    image.parts = &part;
    Require(EXR_OK(exr_save_to_file(path.string().c_str(), &image, EXR_COMPRESSION_NONE)), "Could not save RGBA EXR fixture.");
}

void SaveHalfRgb(const std::filesystem::path& path)
{
    exr_channel channels[3];
    InitializeChannel(channels[0], "B", EXR_PIXEL_HALF);
    InitializeChannel(channels[1], "G", EXR_PIXEL_HALF);
    InitializeChannel(channels[2], "R", EXR_PIXEL_HALF);
    float sourceB[2] = { 0.6f, 0.9f };
    float sourceG[2] = { 0.5f, 0.8f };
    float sourceR[2] = { 0.4f, 0.7f };
    std::uint16_t b[2], g[2], r[2];
    exr_float_to_half(sourceB, b, 2);
    exr_float_to_half(sourceG, g, 2);
    exr_float_to_half(sourceR, r, 2);
    void* images[3] = { b, g, r };
    exr_part part;
    InitializePart(part, channels, images, 3);
    exr_image image{};
    image.num_parts = 1;
    image.parts = &part;
    Require(EXR_OK(exr_save_to_file(path.string().c_str(), &image, EXR_COMPRESSION_NONE)), "Could not save RGB half EXR fixture.");
}

void SaveLuminance(const std::filesystem::path& path)
{
    exr_channel channel;
    InitializeChannel(channel, "Y", EXR_PIXEL_FLOAT);
    float y[2] = { 0.125f, 0.75f };
    void* images[1] = { y };
    exr_part part;
    InitializePart(part, &channel, images, 1);
    exr_image image{};
    image.num_parts = 1;
    image.parts = &part;
    Require(EXR_OK(exr_save_to_file(path.string().c_str(), &image, EXR_COMPRESSION_NONE)), "Could not save Y EXR fixture.");
}

void TestLoaders(const std::filesystem::path& root)
{
    const std::filesystem::path rgbaPath = root / "rgba-float.exr";
    const std::filesystem::path rgbPath = root / "rgb-half.exr";
    const std::filesystem::path yPath = root / "y-float.exr";
    SaveFloatRgba(rgbaPath);
    SaveHalfRgb(rgbPath);
    SaveLuminance(yPath);

    Bistro::ExrImage loaded;
    std::string diagnostics;
    Require(Bistro::LoadExrRgba32Float(rgbaPath.wstring(), loaded, diagnostics), diagnostics.c_str());
    Require(loaded.width == 2 && loaded.height == 1 && loaded.rgba.size() == 8, "RGBA EXR dimensions are wrong.");
    Require(std::abs(loaded.rgba[0] - 0.1f) < 1e-6f && std::abs(loaded.rgba[3] - 0.25f) < 1e-6f, "RGBA channel mapping is wrong.");
    Require(loaded.rgba[4] == 0.0f && loaded.rgba[5] == 0.0f && loaded.rgba[6] == 0.0f && loaded.rgba[7] == 1.0f, "Non-finite EXR values were not sanitized.");

    Require(Bistro::LoadExrRgba32Float(rgbPath.wstring(), loaded, diagnostics), diagnostics.c_str());
    Require(std::abs(loaded.rgba[0] - 0.4f) < 0.001f && loaded.rgba[3] == 1.0f, "Half RGB conversion is wrong.");

    Require(Bistro::LoadExrRgba32Float(yPath.wstring(), loaded, diagnostics), diagnostics.c_str());
    Require(loaded.rgba[0] == loaded.rgba[1] && loaded.rgba[1] == loaded.rgba[2] && loaded.rgba[3] == 1.0f, "Y channel expansion is wrong.");

    const std::filesystem::path corruptPath = root / "corrupt.exr";
    std::ofstream corrupt(corruptPath, std::ios::binary);
    corrupt << "not an exr";
    corrupt.close();
    Require(!Bistro::LoadExrRgba32Float(corruptPath.wstring(), loaded, diagnostics), "Corrupt EXR must fail.");
    Require(diagnostics.find("corrupt.exr") != std::string::npos, "EXR failure must name the source file.");
}
}

int main()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("D3D12LookDevPT-TinyExrLoaderTests-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    try
    {
        TestLoaders(root);
        std::filesystem::remove_all(root);
        std::cout << "TinyEXR loader tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::filesystem::remove_all(root);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}

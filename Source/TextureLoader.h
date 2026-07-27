#pragma once

#include <DirectXTex.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Bistro
{
    inline constexpr uint32_t EnvironmentImportanceMaxDimension = 512u;
    inline constexpr size_t EnvironmentAliasMaxEntryCount = size_t{ 1 } << 24u;

    struct TextureMip
    {
        uint32_t width = 1;
        uint32_t height = 1;
        size_t offset = 0;
        size_t rowPitch = 4;
        size_t slicePitch = 4;
    };

    struct TextureData
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t mipLevels = 1;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool fallback = false;
        std::vector<TextureMip> mips;
        std::vector<uint8_t> pixels;
    };

    struct EnvironmentAliasEntry
    {
        float acceptProbability = 1.0f;
        float aliasIndex = 0.0f;
        float texelProbability = 1.0f;
        float padding = 0.0f;
    };
    static_assert(sizeof(EnvironmentAliasEntry) == 16, "Environment alias texel ABI must remain RGBA32F.");

    TextureData LoadTextureRgba8(
        const std::wstring& path,
        bool srgb,
        const uint8_t fallback[4],
        float alphaCoverageCutoff = -1.0f);
    TextureData LoadTextureD3D12(
        const std::wstring& path,
        bool srgb,
        const uint8_t fallback[4],
        float alphaCoverageCutoff = -1.0f);
    TextureData LoadTextureVulkan(const std::wstring& path, bool srgb, const uint8_t fallback[4], bool preserveBcCompressed);
    // Loads only the lat-long image used to build the environment importance
    // distribution. The result is linear RGBA32F, sanitized, and capped to a
    // 512-pixel maximum edge independently of the renderable environment map.
    TextureData LoadEnvironmentImportanceSource(
        const std::wstring& path,
        const uint8_t fallback[4],
        uint32_t maxDimension = EnvironmentImportanceMaxDimension);
    // Renderable environment radiance follows the same finite, non-negative
    // linear contract as its importance source and includes a mip chain.
    TextureData LoadEnvironmentRadianceTexture(
        const std::wstring& path,
        const uint8_t fallback[4],
        uint32_t maxDimension = EnvironmentImportanceMaxDimension);
    std::vector<EnvironmentAliasEntry> BuildEnvironmentAliasTable(const TextureData& texture);
}

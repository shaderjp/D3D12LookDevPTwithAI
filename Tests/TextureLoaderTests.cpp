#include "TextureLoader.h"

#include <DirectXTex.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <objbase.h>
#include <stdexcept>
#include <vector>
#include <wincodec.h>

namespace
{
    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void RequireSucceeded(HRESULT result, const char* message)
    {
        if (FAILED(result))
        {
            throw std::runtime_error(message);
        }
    }

    class ScopedComApartment
    {
    public:
        ScopedComApartment()
            : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
        {
        }

        ~ScopedComApartment()
        {
            if (SUCCEEDED(m_result))
            {
                CoUninitialize();
            }
        }

        HRESULT Result() const
        {
            return m_result;
        }

    private:
        HRESULT m_result;
    };

    void ValidateLinearRadiance(
        const Bistro::TextureData& texture,
        uint32_t expectedWidth,
        uint32_t expectedHeight,
        bool expectMipChain,
        const char* context)
    {
        Require(!texture.fallback, context);
        Require(texture.format == DXGI_FORMAT_R32G32B32A32_FLOAT, context);
        Require(texture.width == expectedWidth && texture.height == expectedHeight, context);
        Require(texture.width <= Bistro::EnvironmentImportanceMaxDimension &&
            texture.height <= Bistro::EnvironmentImportanceMaxDimension, context);
        Require(static_cast<size_t>(texture.width) <=
            Bistro::EnvironmentAliasMaxEntryCount / static_cast<size_t>(texture.height), context);
        Require(static_cast<size_t>(texture.width) * texture.height <=
            Bistro::EnvironmentAliasMaxEntryCount, context);
        Require(expectMipChain ? texture.mips.size() > 1 : texture.mips.size() == 1, context);

        for (const Bistro::TextureMip& mip : texture.mips)
        {
            const size_t tightRowPitch = static_cast<size_t>(mip.width) * sizeof(float) * 4;
            Require(mip.width > 0 && mip.height > 0 && mip.rowPitch >= tightRowPitch, context);
            Require(mip.offset <= texture.pixels.size() &&
                mip.slicePitch <= texture.pixels.size() - mip.offset, context);
            for (uint32_t y = 0; y < mip.height; ++y)
            {
                const float* row = reinterpret_cast<const float*>(
                    texture.pixels.data() + mip.offset + static_cast<size_t>(y) * mip.rowPitch);
                for (uint32_t x = 0; x < mip.width; ++x)
                {
                    const float* pixel = row + static_cast<size_t>(x) * 4;
                    for (size_t channel = 0; channel < 3; ++channel)
                    {
                        Require(std::isfinite(pixel[channel]) && pixel[channel] >= 0.0f, context);
                    }
                }
            }
        }
    }

    void RequireMipZeroMatches(
        const Bistro::TextureData& importance,
        const Bistro::TextureData& radiance,
        const char* context)
    {
        Require(!importance.mips.empty() && !radiance.mips.empty(), context);
        const Bistro::TextureMip& expected = importance.mips.front();
        const Bistro::TextureMip& actual = radiance.mips.front();
        Require(expected.width == actual.width && expected.height == actual.height, context);
        const size_t rowBytes = static_cast<size_t>(expected.width) * sizeof(float) * 4;
        for (uint32_t y = 0; y < expected.height; ++y)
        {
            const uint8_t* expectedRow = importance.pixels.data() + expected.offset +
                static_cast<size_t>(y) * expected.rowPitch;
            const uint8_t* actualRow = radiance.pixels.data() + actual.offset +
                static_cast<size_t>(y) * actual.rowPitch;
            Require(std::memcmp(expectedRow, actualRow, rowBytes) == 0, context);
        }
    }

    void WriteU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    void WriteU64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value)
    {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    void WriteNativeBc7Ktx2(const std::filesystem::path& path, bool truncateSecondMip)
    {
        constexpr uint8_t identifier[12] = {
            0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
        constexpr size_t levelIndexEnd = 80 + 2 * 24;
        constexpr size_t mip0Bytes = 64;
        constexpr size_t mip1Bytes = 16;
        std::vector<uint8_t> bytes(levelIndexEnd + mip0Bytes + (truncateSecondMip ? 8 : mip1Bytes), 0);
        std::memcpy(bytes.data(), identifier, sizeof(identifier));
        WriteU32(bytes, 12, 145); // VK_FORMAT_BC7_UNORM_BLOCK
        WriteU32(bytes, 16, 1);
        WriteU32(bytes, 20, 8);
        WriteU32(bytes, 24, 8);
        WriteU32(bytes, 36, 1);
        WriteU32(bytes, 40, 2);
        WriteU64(bytes, 80, levelIndexEnd);
        WriteU64(bytes, 88, mip0Bytes);
        WriteU64(bytes, 96, mip0Bytes);
        WriteU64(bytes, 104, levelIndexEnd + mip0Bytes);
        WriteU64(bytes, 112, mip1Bytes);
        WriteU64(bytes, 120, mip1Bytes);
        for (size_t index = levelIndexEnd; index < bytes.size(); ++index)
        {
            bytes[index] = static_cast<uint8_t>(index & 0xffu);
        }
        std::ofstream output(path, std::ios::binary);
        Require(output && static_cast<bool>(output.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))),
            "failed to write native KTX2 fixture");
    }
}

int wmain()
{
    std::vector<std::filesystem::path> temporaryPaths;
    try
    {
        const ScopedComApartment com;
        RequireSucceeded(com.Result(), "failed to initialize COM for WIC tests");

        const auto uniqueId = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::wstring baseName = L"D3D12LookDevPT-TextureLoaderTests-" + std::to_wstring(uniqueId);
        const std::filesystem::path temporaryDirectory = std::filesystem::temp_directory_path();
        const std::filesystem::path ddsPath = temporaryDirectory / (baseName + L".dds");
        const std::filesystem::path hdrPath = temporaryDirectory / (baseName + L".hdr");
        const std::filesystem::path wicPath = temporaryDirectory / (baseName + L".png");
        const std::filesystem::path ktx2Path = temporaryDirectory / (baseName + L".ktx2");
        const std::filesystem::path brokenKtx2Path = temporaryDirectory / (baseName + L"-broken.ktx2");
        temporaryPaths = { ddsPath, hdrPath, wicPath, ktx2Path, brokenKtx2Path };

        DirectX::ScratchImage ddsSource;
        RequireSucceeded(
            ddsSource.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, 1024, 256, 1, 1),
            "failed to initialize test DDS");
        const DirectX::Image* ddsImage = ddsSource.GetImage(0, 0, 0);
        Require(ddsImage != nullptr, "test DDS image is missing");
        for (size_t y = 0; y < ddsImage->height; ++y)
        {
            float* row = reinterpret_cast<float*>(ddsImage->pixels + y * ddsImage->rowPitch);
            for (size_t x = 0; x < ddsImage->width; ++x)
            {
                float* pixel = row + x * 4;
                pixel[0] = x < 64 ? std::numeric_limits<float>::quiet_NaN() : 2.0f;
                pixel[1] = x < 64 ? std::numeric_limits<float>::infinity() : 1.0f;
                pixel[2] = x < 64 ? -1.0f : 0.5f;
                pixel[3] = 1.0f;
            }
        }
        RequireSucceeded(
            DirectX::SaveToDDSFile(*ddsImage, DirectX::DDS_FLAGS_NONE, ddsPath.c_str()),
            "failed to write test DDS");

        DirectX::ScratchImage hdrSource;
        RequireSucceeded(
            hdrSource.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, 1024, 256, 1, 1),
            "failed to initialize test HDR");
        const DirectX::Image* hdrImage = hdrSource.GetImage(0, 0, 0);
        Require(hdrImage != nullptr, "test HDR image is missing");
        for (size_t y = 0; y < hdrImage->height; ++y)
        {
            float* row = reinterpret_cast<float*>(hdrImage->pixels + y * hdrImage->rowPitch);
            for (size_t x = 0; x < hdrImage->width; ++x)
            {
                float* pixel = row + x * 4;
                pixel[0] = 0.25f + static_cast<float>(x) / static_cast<float>(hdrImage->width);
                pixel[1] = 0.5f + static_cast<float>(y) / static_cast<float>(hdrImage->height);
                pixel[2] = 2.0f;
                pixel[3] = 1.0f;
            }
        }
        RequireSucceeded(DirectX::SaveToHDRFile(*hdrImage, hdrPath.c_str()), "failed to write test HDR");

        DirectX::ScratchImage wicSource;
        RequireSucceeded(
            wicSource.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 1024, 256, 1, 1),
            "failed to initialize test WIC image");
        const DirectX::Image* wicImage = wicSource.GetImage(0, 0, 0);
        Require(wicImage != nullptr, "test WIC image is missing");
        for (size_t y = 0; y < wicImage->height; ++y)
        {
            uint8_t* row = wicImage->pixels + y * wicImage->rowPitch;
            for (size_t x = 0; x < wicImage->width; ++x)
            {
                uint8_t* pixel = row + x * 4;
                pixel[0] = static_cast<uint8_t>(x & 0xffu);
                pixel[1] = static_cast<uint8_t>(y & 0xffu);
                pixel[2] = 127;
                pixel[3] = 255;
            }
        }
        RequireSucceeded(
            DirectX::SaveToWICFile(
                *wicImage,
                DirectX::WIC_FLAGS_NONE,
                GUID_ContainerFormatPng,
                wicPath.c_str()),
            "failed to write test WIC image");

        WriteNativeBc7Ktx2(ktx2Path, false);
        WriteNativeBc7Ktx2(brokenKtx2Path, true);

        const uint8_t fallback[4] = { 0, 0, 0, 255 };
        const Bistro::TextureData nativeKtx2 = Bistro::LoadTextureD3D12(
            ktx2Path.wstring(), true, fallback, -1.0f, 4);
        Require(!nativeKtx2.fallback && nativeKtx2.container == "ktx2-native",
            "native KTX2 did not use the direct-upload path");
        Require(nativeKtx2.format == DXGI_FORMAT_BC7_UNORM_SRGB &&
            nativeKtx2.sourceWidth == 8 && nativeKtx2.sourceHeight == 8 &&
            nativeKtx2.width == 4 && nativeKtx2.height == 4 &&
            nativeKtx2.mips.size() == 1 && nativeKtx2.residentBytes == 16,
            "native KTX2 resident mip selection is invalid");
        const Bistro::TextureData brokenKtx2 = Bistro::LoadTextureD3D12(
            brokenKtx2Path.wstring(), false, fallback, -1.0f, 8);
        Require(brokenKtx2.fallback, "truncated KTX2 mip payload was accepted");
        const auto verifyEnvironmentPair = [&](const std::filesystem::path& path, const char* context)
        {
            const Bistro::TextureData importance = Bistro::LoadEnvironmentImportanceSource(
                path.wstring(), fallback, 4096);
            const Bistro::TextureData radiance = Bistro::LoadEnvironmentRadianceTexture(
                path.wstring(), fallback, 4096);
            ValidateLinearRadiance(importance, 1024, 256, false, context);
            ValidateLinearRadiance(radiance, 1024, 256, true, context);
            RequireMipZeroMatches(importance, radiance, context);
            return importance;
        };

        const Bistro::TextureData loaded = verifyEnvironmentPair(ddsPath, "DDS environment contract failed");
        const Bistro::TextureMip& ddsMip = loaded.mips.front();
        const float* firstDdsPixel = reinterpret_cast<const float*>(loaded.pixels.data() + ddsMip.offset);
        Require(firstDdsPixel[0] == 0.0f && firstDdsPixel[1] == 0.0f && firstDdsPixel[2] == 0.0f,
            "DDS NaN/Inf/negative radiance was not sanitized to zero before resize");
        (void)verifyEnvironmentPair(hdrPath, "HDR environment contract failed");
        (void)verifyEnvironmentPair(wicPath, "WIC environment contract failed");

        const std::vector<Bistro::EnvironmentAliasEntry> alias = Bistro::BuildEnvironmentAliasTable(loaded);
        Require(alias.size() == static_cast<size_t>(loaded.width) * loaded.height, "alias table dimensions are incorrect");
        double probabilitySum = 0.0;
        for (const Bistro::EnvironmentAliasEntry& entry : alias)
        {
            Require(std::isfinite(entry.acceptProbability) && entry.acceptProbability >= 0.0f && entry.acceptProbability <= 1.0f,
                "alias acceptance probability is invalid");
            Require(std::isfinite(entry.aliasIndex) && entry.aliasIndex >= 0.0f &&
                entry.aliasIndex < static_cast<float>(alias.size()), "alias index is invalid");
            Require(std::isfinite(entry.texelProbability) && entry.texelProbability >= 0.0f,
                "realized environment PMF is invalid");
            probabilitySum += entry.texelProbability;
        }
        Require(std::abs(probabilitySum - 1.0) < 1.0e-5, "realized environment PMF is not normalized");

        Bistro::TextureData constantEnvironment;
        constantEnvironment.width = 1;
        constantEnvironment.height = 4;
        constantEnvironment.mipLevels = 1;
        constantEnvironment.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        constantEnvironment.mips.push_back({ 1, 4, 0, sizeof(float) * 4, sizeof(float) * 16 });
        constantEnvironment.pixels.resize(sizeof(float) * 16);
        float* constantPixels = reinterpret_cast<float*>(constantEnvironment.pixels.data());
        for (size_t pixel = 0; pixel < 4; ++pixel)
        {
            constantPixels[pixel * 4 + 0] = 1.0f;
            constantPixels[pixel * 4 + 1] = 1.0f;
            constantPixels[pixel * 4 + 2] = 1.0f;
            constantPixels[pixel * 4 + 3] = 1.0f;
        }
        const auto equalAreaAlias = Bistro::BuildEnvironmentAliasTable(constantEnvironment, true);
        const auto latLongAlias = Bistro::BuildEnvironmentAliasTable(constantEnvironment, false);
        for (const Bistro::EnvironmentAliasEntry& entry : equalAreaAlias)
        {
            Require(std::abs(entry.texelProbability - 0.25f) < 1.0e-6f,
                "equal-area environment texels did not receive equal solid-angle weight");
        }
        Require(latLongAlias.front().texelProbability < latLongAlias[1].texelProbability,
            "legacy lat-long environment weighting lost its polar Jacobian");

        Bistro::TextureData oversized;
        oversized.width = 4097;
        oversized.height = 4097;
        bool rejectedOversizedTable = false;
        try
        {
            (void)Bistro::BuildEnvironmentAliasTable(oversized);
        }
        catch (const std::runtime_error&)
        {
            rejectedOversizedTable = true;
        }
        Require(rejectedOversizedTable, "alias table larger than 2^24 entries was accepted");

        const Bistro::TextureData invalidLimit = Bistro::LoadEnvironmentImportanceSource(
            ddsPath.wstring(), fallback, 0);
        Require(invalidLimit.fallback && invalidLimit.width == 1 && invalidLimit.height == 1,
            "zero maximum dimension did not produce a safe fallback");

        for (const std::filesystem::path& path : temporaryPaths)
        {
            std::filesystem::remove(path);
        }
        std::cout << "TextureLoaderTests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        for (const std::filesystem::path& path : temporaryPaths)
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
        std::cerr << "TextureLoaderTests failed: " << exception.what() << '\n';
        return 1;
    }
}

#include "TextureLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace
{
    constexpr size_t MaxTextureDimension = 512;

    Bistro::TextureData MakeFallbackTexture(bool srgb, const uint8_t fallback[4])
    {
        Bistro::TextureData texture;
        texture.width = 1;
        texture.height = 1;
        texture.mipLevels = 1;
        texture.format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.fallback = true;
        texture.mips.push_back({ 1, 1, 0, 4, 4 });
        texture.pixels.assign(fallback, fallback + 4);
        return texture;
    }

    Bistro::TextureData MakeLinearRadianceFallbackTexture(const uint8_t fallback[4])
    {
        Bistro::TextureData texture;
        texture.width = 1;
        texture.height = 1;
        texture.mipLevels = 1;
        texture.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        texture.fallback = true;
        texture.mips.push_back({ 1, 1, 0, sizeof(float) * 4, sizeof(float) * 4 });
        texture.pixels.resize(sizeof(float) * 4);

        float rgba[4] =
        {
            static_cast<float>(fallback[0]) / 255.0f,
            static_cast<float>(fallback[1]) / 255.0f,
            static_cast<float>(fallback[2]) / 255.0f,
            static_cast<float>(fallback[3]) / 255.0f,
        };
        memcpy(texture.pixels.data(), rgba, sizeof(rgba));
        return texture;
    }

    Bistro::TextureData MakeTextureFromImages(const DirectX::Image* images, size_t imageCount, DXGI_FORMAT format)
    {
        if (images == nullptr || imageCount == 0 || images[0].width == 0 || images[0].height == 0 || images[0].pixels == nullptr)
        {
            throw std::runtime_error("Invalid texture image data.");
        }

        Bistro::TextureData texture;
        texture.width = static_cast<uint32_t>(images[0].width);
        texture.height = static_cast<uint32_t>(images[0].height);
        texture.mipLevels = static_cast<uint32_t>(imageCount);
        texture.format = format;
        texture.mips.reserve(imageCount);

        for (size_t mipIndex = 0; mipIndex < imageCount; ++mipIndex)
        {
            const DirectX::Image& image = images[mipIndex];
            const size_t rowSize = image.width * 4;
            const size_t slicePitch = rowSize * image.height;
            const size_t offset = texture.pixels.size();
            texture.pixels.resize(offset + slicePitch);

            for (size_t y = 0; y < image.height; ++y)
            {
                memcpy(texture.pixels.data() + offset + y * rowSize, image.pixels + y * image.rowPitch, rowSize);
            }

            texture.mips.push_back({
                static_cast<uint32_t>(image.width),
                static_cast<uint32_t>(image.height),
                offset,
                rowSize,
                slicePitch
            });
        }

        return texture;
    }

    Bistro::TextureData MakeTextureFromImageMemory(const DirectX::Image* images, size_t imageCount, DXGI_FORMAT format)
    {
        if (images == nullptr || imageCount == 0 || images[0].width == 0 || images[0].height == 0 || images[0].pixels == nullptr)
        {
            throw std::runtime_error("Invalid texture image data.");
        }

        Bistro::TextureData texture;
        texture.width = static_cast<uint32_t>(images[0].width);
        texture.height = static_cast<uint32_t>(images[0].height);
        texture.mipLevels = static_cast<uint32_t>(imageCount);
        texture.format = format;
        texture.mips.reserve(imageCount);

        for (size_t mipIndex = 0; mipIndex < imageCount; ++mipIndex)
        {
            const DirectX::Image& image = images[mipIndex];
            const size_t offset = texture.pixels.size();
            texture.pixels.resize(offset + image.slicePitch);
            memcpy(texture.pixels.data() + offset, image.pixels, image.slicePitch);
            texture.mips.push_back({
                static_cast<uint32_t>(image.width),
                static_cast<uint32_t>(image.height),
                offset,
                image.rowPitch,
                image.slicePitch
            });
        }

        return texture;
    }

    bool IsRgba8Format(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }

    double AlphaCoverage(const Bistro::TextureData& texture, const Bistro::TextureMip& mip, float cutoff, float scale)
    {
        if (!IsRgba8Format(texture.format) || mip.width == 0 || mip.height == 0)
        {
            return 0.0;
        }

        size_t covered = 0;
        const size_t sampleCount = static_cast<size_t>(mip.width) * mip.height;
        for (uint32_t y = 0; y < mip.height; ++y)
        {
            const uint8_t* row = texture.pixels.data() + mip.offset + static_cast<size_t>(y) * mip.rowPitch;
            for (uint32_t x = 0; x < mip.width; ++x)
            {
                const float alpha = static_cast<float>(row[static_cast<size_t>(x) * 4 + 3]) / 255.0f;
                covered += std::min(alpha * scale, 1.0f) >= cutoff ? 1u : 0u;
            }
        }
        return sampleCount > 0 ? static_cast<double>(covered) / static_cast<double>(sampleCount) : 0.0;
    }

    void PreserveAlphaCoverage(Bistro::TextureData& texture, float cutoff)
    {
        if (!IsRgba8Format(texture.format) || texture.mips.size() <= 1 || cutoff <= 0.0f || cutoff > 1.0f)
        {
            return;
        }

        const double targetCoverage = AlphaCoverage(texture, texture.mips.front(), cutoff, 1.0f);
        if (targetCoverage <= 0.0)
        {
            return;
        }

        for (size_t mipIndex = 1; mipIndex < texture.mips.size(); ++mipIndex)
        {
            const Bistro::TextureMip& mip = texture.mips[mipIndex];
            float low = 0.0f;
            float high = 1.0f;
            while (AlphaCoverage(texture, mip, cutoff, high) < targetCoverage && high < 64.0f)
            {
                high *= 2.0f;
            }

            // Find the smallest alpha scale whose thresholded coverage reaches
            // mip-0 coverage. This is the standard coverage-preserving alpha
            // mip adjustment used for cutout foliage and fine wires.
            for (uint32_t iteration = 0; iteration < 18u; ++iteration)
            {
                const float middle = (low + high) * 0.5f;
                if (AlphaCoverage(texture, mip, cutoff, middle) < targetCoverage)
                {
                    low = middle;
                }
                else
                {
                    high = middle;
                }
            }

            for (uint32_t y = 0; y < mip.height; ++y)
            {
                uint8_t* row = texture.pixels.data() + mip.offset + static_cast<size_t>(y) * mip.rowPitch;
                for (uint32_t x = 0; x < mip.width; ++x)
                {
                    uint8_t& alpha = row[static_cast<size_t>(x) * 4 + 3];
                    const float adjusted = std::min(static_cast<float>(alpha) / 255.0f * high, 1.0f);
                    alpha = static_cast<uint8_t>(std::lround(adjusted * 255.0f));
                }
            }
        }
    }

    HRESULT LoadTextureFile(
        const std::wstring& path,
        DirectX::TexMetadata& metadata,
        DirectX::ScratchImage& image)
    {
        const std::filesystem::path fsPath(path);
        if (_wcsicmp(fsPath.extension().c_str(), L".hdr") == 0)
        {
            return DirectX::LoadFromHDRFile(path.c_str(), &metadata, image);
        }
        if (_wcsicmp(fsPath.extension().c_str(), L".dds") == 0)
        {
            return DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
        }
        if (_wcsicmp(fsPath.extension().c_str(), L".tga") == 0)
        {
            return DirectX::LoadFromTGAFile(path.c_str(), &metadata, image);
        }
        return DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, image);
    }

    bool SanitizeLinearRadiance(const DirectX::Image* image)
    {
        if (image == nullptr || image->pixels == nullptr || image->width == 0 || image->height == 0 ||
            image->format != DXGI_FORMAT_R32G32B32A32_FLOAT || image->rowPitch < image->width * sizeof(float) * 4)
        {
            return false;
        }

        for (size_t y = 0; y < image->height; ++y)
        {
            float* row = reinterpret_cast<float*>(image->pixels + y * image->rowPitch);
            for (size_t x = 0; x < image->width; ++x)
            {
                float* pixel = row + x * 4;
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    if (!std::isfinite(pixel[channel]) || pixel[channel] < 0.0f)
                    {
                        pixel[channel] = 0.0f;
                    }
                }
            }
        }
        return true;
    }

    Bistro::TextureData LoadLinearRadianceTexture(
        const std::wstring& path,
        const uint8_t fallback[4],
        uint32_t requestedMaxDimension,
        bool generateMipChain)
    {
        if (path.empty() || !std::filesystem::exists(path) || requestedMaxDimension == 0)
        {
            return MakeLinearRadianceFallbackTexture(fallback);
        }

        const uint32_t maxDimension = std::min(
            requestedMaxDimension,
            Bistro::EnvironmentImportanceMaxDimension);
        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage image;
        HRESULT hr = LoadTextureFile(path, metadata, image);
        if (FAILED(hr) || image.GetImageCount() == 0)
        {
            return MakeLinearRadianceFallbackTexture(fallback);
        }

        const DirectX::Image* source = image.GetImage(0, 0, 0);
        if (source == nullptr)
        {
            return MakeLinearRadianceFallbackTexture(fallback);
        }

        DirectX::ScratchImage decompressed;
        DirectX::ScratchImage converted;
        if (DirectX::IsCompressed(source->format))
        {
            hr = DirectX::Decompress(*source, DXGI_FORMAT_R32G32B32A32_FLOAT, decompressed);
            if (FAILED(hr) || decompressed.GetImageCount() == 0)
            {
                return MakeLinearRadianceFallbackTexture(fallback);
            }
            source = decompressed.GetImage(0, 0, 0);
        }
        else if (source->format != DXGI_FORMAT_R32G32B32A32_FLOAT)
        {
            hr = DirectX::Convert(
                *source,
                DXGI_FORMAT_R32G32B32A32_FLOAT,
                DirectX::TEX_FILTER_DEFAULT,
                0.0f,
                converted);
            if (FAILED(hr) || converted.GetImageCount() == 0)
            {
                return MakeLinearRadianceFallbackTexture(fallback);
            }
            source = converted.GetImage(0, 0, 0);
        }

        // Invalid radiance must be removed before filtering; otherwise a
        // single NaN/Inf can contaminate an entire mip chain or alias table.
        if (!SanitizeLinearRadiance(source))
        {
            return MakeLinearRadianceFallbackTexture(fallback);
        }

        DirectX::ScratchImage resized;
        if (source->width > maxDimension || source->height > maxDimension)
        {
            const double scale = static_cast<double>(maxDimension) /
                static_cast<double>((std::max)(source->width, source->height));
            const size_t width = (std::max<size_t>)(1, static_cast<size_t>(static_cast<double>(source->width) * scale));
            const size_t height = (std::max<size_t>)(1, static_cast<size_t>(static_cast<double>(source->height) * scale));
            hr = DirectX::Resize(*source, width, height, DirectX::TEX_FILTER_DEFAULT, resized);
            if (FAILED(hr) || resized.GetImageCount() == 0)
            {
                return MakeLinearRadianceFallbackTexture(fallback);
            }
            source = resized.GetImage(0, 0, 0);
            if (!SanitizeLinearRadiance(source))
            {
                return MakeLinearRadianceFallbackTexture(fallback);
            }
        }

        DirectX::ScratchImage mipChain;
        if (generateMipChain)
        {
            hr = DirectX::GenerateMipMaps(*source, DirectX::TEX_FILTER_DEFAULT, 0, mipChain);
        }
        try
        {
            if (generateMipChain && SUCCEEDED(hr) && mipChain.GetImageCount() > 0)
            {
                for (size_t imageIndex = 0; imageIndex < mipChain.GetImageCount(); ++imageIndex)
                {
                    if (!SanitizeLinearRadiance(mipChain.GetImages() + imageIndex))
                    {
                        return MakeLinearRadianceFallbackTexture(fallback);
                    }
                }
                return MakeTextureFromImageMemory(
                    mipChain.GetImages(),
                    mipChain.GetImageCount(),
                    DXGI_FORMAT_R32G32B32A32_FLOAT);
            }
            return MakeTextureFromImageMemory(source, 1, DXGI_FORMAT_R32G32B32A32_FLOAT);
        }
        catch (const std::runtime_error&)
        {
            return MakeLinearRadianceFallbackTexture(fallback);
        }
    }

    Bistro::TextureData LoadHdrTexture(const std::wstring& path)
    {
        const uint8_t fallback[] = { 0, 0, 0, 255 };
        return LoadLinearRadianceTexture(path, fallback, Bistro::EnvironmentImportanceMaxDimension, true);
    }

    DXGI_FORMAT ToSrgbFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_BC1_UNORM:
            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_UNORM:
            return DXGI_FORMAT_BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_UNORM:
            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC7_UNORM:
            return DXGI_FORMAT_BC7_UNORM_SRGB;
        default:
            return format;
        }
    }

    DXGI_FORMAT ToLinearFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_BC1_UNORM_SRGB:
            return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_UNORM_SRGB:
            return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_UNORM_SRGB:
            return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return DXGI_FORMAT_BC7_UNORM;
        default:
            return format;
        }
    }

    bool IsVulkanPreservableFormat(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }
}

namespace Bistro
{
    TextureData LoadTextureRgba8(const std::wstring& path, bool srgb, const uint8_t fallback[4], float alphaCoverageCutoff)
    {
        if (path.empty() || !std::filesystem::exists(path))
        {
            return MakeFallbackTexture(srgb, fallback);
        }

        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage image;
        HRESULT hr = E_FAIL;
        std::filesystem::path fsPath(path);
        if (_wcsicmp(fsPath.extension().c_str(), L".hdr") == 0)
        {
            return LoadHdrTexture(path);
        }
        if (_wcsicmp(fsPath.extension().c_str(), L".dds") == 0)
        {
            hr = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
        }
        else if (_wcsicmp(fsPath.extension().c_str(), L".tga") == 0)
        {
            hr = DirectX::LoadFromTGAFile(path.c_str(), &metadata, image);
        }
        else
        {
            hr = DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_NONE, &metadata, image);
        }

        if (FAILED(hr))
        {
            return MakeFallbackTexture(srgb, fallback);
        }

        DirectX::ScratchImage decompressed;
        const DirectX::Image* source = image.GetImage(0, 0, 0);
        if (DirectX::IsCompressed(metadata.format))
        {
            hr = DirectX::Decompress(*source, DXGI_FORMAT_UNKNOWN, decompressed);
            if (FAILED(hr))
            {
                return MakeFallbackTexture(srgb, fallback);
            }
            source = decompressed.GetImage(0, 0, 0);
        }

        DirectX::ScratchImage converted;
        const DXGI_FORMAT outputFormat = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = DirectX::Convert(*source, outputFormat, DirectX::TEX_FILTER_DEFAULT, 0.0f, converted);
        if (FAILED(hr))
        {
            return MakeFallbackTexture(srgb, fallback);
        }

        source = converted.GetImage(0, 0, 0);
        DirectX::ScratchImage resized;
        if (source->width > MaxTextureDimension || source->height > MaxTextureDimension)
        {
            const float scale = static_cast<float>(MaxTextureDimension) / static_cast<float>((std::max)(source->width, source->height));
            const size_t width = (std::max<size_t>)(1, static_cast<size_t>(source->width * scale));
            const size_t height = (std::max<size_t>)(1, static_cast<size_t>(source->height * scale));
            hr = DirectX::Resize(*source, width, height, DirectX::TEX_FILTER_DEFAULT, resized);
            if (FAILED(hr))
            {
                return MakeFallbackTexture(srgb, fallback);
            }
            source = resized.GetImage(0, 0, 0);
        }

        if (source->width == 0 || source->height == 0 || source->pixels == nullptr)
        {
            return MakeFallbackTexture(srgb, fallback);
        }

        DirectX::ScratchImage mipChain;
        hr = DirectX::GenerateMipMaps(*source, DirectX::TEX_FILTER_DEFAULT, 0, mipChain);
        try
        {
            if (SUCCEEDED(hr) && mipChain.GetImageCount() > 0)
            {
                TextureData texture = MakeTextureFromImages(mipChain.GetImages(), mipChain.GetImageCount(), outputFormat);
                PreserveAlphaCoverage(texture, alphaCoverageCutoff);
                return texture;
            }

            TextureData texture = MakeTextureFromImages(source, 1, outputFormat);
            PreserveAlphaCoverage(texture, alphaCoverageCutoff);
            return texture;
        }
        catch (const std::runtime_error&)
        {
            return MakeFallbackTexture(srgb, fallback);
        }
    }

    TextureData LoadTextureD3D12(const std::wstring& path, bool srgb, const uint8_t fallback[4], float alphaCoverageCutoff)
    {
        if (path.empty() || !std::filesystem::exists(path))
        {
            return MakeFallbackTexture(srgb, fallback);
        }

        std::filesystem::path fsPath(path);
        if (_wcsicmp(fsPath.extension().c_str(), L".hdr") == 0)
        {
            return LoadHdrTexture(path);
        }
        if (_wcsicmp(fsPath.extension().c_str(), L".dds") != 0)
        {
            return LoadTextureRgba8(path, srgb, fallback, alphaCoverageCutoff);
        }
        if (alphaCoverageCutoff >= 0.0f)
        {
            // Coverage preservation requires writable uncompressed alpha. DDS
            // cutouts are decoded to RGBA8 instead of retaining BC blocks.
            return LoadTextureRgba8(path, srgb, fallback, alphaCoverageCutoff);
        }

        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage image;
        HRESULT hr = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
        if (FAILED(hr) || image.GetImageCount() == 0)
        {
            return MakeFallbackTexture(srgb, fallback);
        }

        const DXGI_FORMAT outputFormat = srgb ? ToSrgbFormat(metadata.format) : ToLinearFormat(metadata.format);
        try
        {
            return MakeTextureFromImageMemory(image.GetImages(), image.GetImageCount(), outputFormat);
        }
        catch (const std::runtime_error&)
        {
            return MakeFallbackTexture(srgb, fallback);
        }
    }

    TextureData LoadTextureVulkan(const std::wstring& path, bool srgb, const uint8_t fallback[4], bool preserveBcCompressed)
    {
        if (path.empty() || !std::filesystem::exists(path))
        {
            return MakeFallbackTexture(srgb, fallback);
        }

        std::filesystem::path fsPath(path);
        if (_wcsicmp(fsPath.extension().c_str(), L".hdr") == 0)
        {
            return LoadHdrTexture(path);
        }
        if (_wcsicmp(fsPath.extension().c_str(), L".dds") != 0)
        {
            return LoadTextureRgba8(path, srgb, fallback);
        }

        DirectX::TexMetadata metadata{};
        DirectX::ScratchImage image;
        HRESULT hr = DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, &metadata, image);
        if (FAILED(hr) || image.GetImageCount() == 0)
        {
            return MakeFallbackTexture(srgb, fallback);
        }

        const DXGI_FORMAT outputFormat = srgb ? ToSrgbFormat(metadata.format) : ToLinearFormat(metadata.format);
        const bool canPreserve = IsVulkanPreservableFormat(outputFormat) && (!DirectX::IsCompressed(outputFormat) || preserveBcCompressed);
        if (!canPreserve)
        {
            return LoadTextureRgba8(path, srgb, fallback);
        }

        try
        {
            return MakeTextureFromImageMemory(image.GetImages(), image.GetImageCount(), outputFormat);
        }
        catch (const std::runtime_error&)
        {
            return MakeFallbackTexture(srgb, fallback);
        }
    }

    TextureData LoadEnvironmentImportanceSource(
        const std::wstring& path,
        const uint8_t fallback[4],
        uint32_t maxDimension)
    {
        return LoadLinearRadianceTexture(path, fallback, maxDimension, false);
    }

    TextureData LoadEnvironmentRadianceTexture(
        const std::wstring& path,
        const uint8_t fallback[4],
        uint32_t maxDimension)
    {
        return LoadLinearRadianceTexture(path, fallback, maxDimension, true);
    }

    std::vector<EnvironmentAliasEntry> BuildEnvironmentAliasTable(const TextureData& texture)
    {
        static_assert(std::numeric_limits<float>::digits >= 24, "Environment alias indices require exact 24-bit float integers.");
        const uint32_t width = std::max(texture.width, 1u);
        const uint32_t height = std::max(texture.height, 1u);
        if (static_cast<size_t>(width) > EnvironmentAliasMaxEntryCount / static_cast<size_t>(height))
        {
            throw std::runtime_error("Environment alias table exceeds its exact 24-bit float index encoding.");
        }
        const size_t entryCount = static_cast<size_t>(width) * height;
        if (entryCount > EnvironmentAliasMaxEntryCount)
        {
            throw std::runtime_error("Environment alias table exceeds its exact 24-bit float index encoding.");
        }
        std::vector<EnvironmentAliasEntry> entries(entryCount);
        if (texture.mips.empty() || texture.pixels.empty())
        {
            const float uniformProbability = 1.0f / static_cast<float>(entryCount);
            for (size_t index = 0; index < entryCount; ++index)
            {
                entries[index].aliasIndex = static_cast<float>(index);
                entries[index].texelProbability = uniformProbability;
            }
            return entries;
        }

        const TextureMip& mip = texture.mips.front();
        if (mip.width != width || mip.height != height || mip.offset > texture.pixels.size() ||
            mip.slicePitch > texture.pixels.size() - mip.offset)
        {
            throw std::runtime_error("Environment importance source metadata is inconsistent with mip 0.");
        }
        DirectX::Image source{};
        source.width = mip.width;
        source.height = mip.height;
        source.format = texture.format;
        source.rowPitch = mip.rowPitch;
        source.slicePitch = mip.slicePitch;
        source.pixels = const_cast<uint8_t*>(texture.pixels.data() + mip.offset);

        DirectX::ScratchImage decompressed;
        DirectX::ScratchImage converted;
        const DirectX::Image* linear = &source;
        HRESULT hr = S_OK;
        if (DirectX::IsCompressed(source.format))
        {
            hr = DirectX::Decompress(source, DXGI_FORMAT_R32G32B32A32_FLOAT, decompressed);
            if (SUCCEEDED(hr))
            {
                linear = decompressed.GetImage(0, 0, 0);
            }
        }
        else if (source.format != DXGI_FORMAT_R32G32B32A32_FLOAT)
        {
            hr = DirectX::Convert(source, DXGI_FORMAT_R32G32B32A32_FLOAT, DirectX::TEX_FILTER_DEFAULT, 0.0f, converted);
            if (SUCCEEDED(hr))
            {
                linear = converted.GetImage(0, 0, 0);
            }
        }

        std::vector<double> weights(entryCount, 1.0);
        double totalWeight = static_cast<double>(entryCount);
        if (SUCCEEDED(hr) && linear && linear->pixels && linear->width == width && linear->height == height)
        {
            totalWeight = 0.0;
            for (uint32_t y = 0; y < height; ++y)
            {
                constexpr float Pi = 3.14159265358979323846f;
                const float theta = Pi * (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
                const double sinTheta = std::max(static_cast<double>(std::sin(theta)), 0.0);
                const float* row = reinterpret_cast<const float*>(linear->pixels + static_cast<size_t>(y) * linear->rowPitch);
                for (uint32_t x = 0; x < width; ++x)
                {
                    const float* pixel = row + static_cast<size_t>(x) * 4;
                    const auto finiteNonNegative = [](float value)
                    {
                        return std::isfinite(value) && value > 0.0f ? static_cast<double>(value) : 0.0;
                    };
                    const double luminance =
                        0.2126 * finiteNonNegative(pixel[0])
                        + 0.7152 * finiteNonNegative(pixel[1])
                        + 0.0722 * finiteNonNegative(pixel[2]);
                    const size_t index = static_cast<size_t>(y) * width + x;
                    weights[index] = luminance * sinTheta;
                    totalWeight += weights[index];
                }
            }
        }

        if (!(totalWeight > 0.0) || !std::isfinite(totalWeight))
        {
            totalWeight = static_cast<double>(entryCount);
            std::fill(weights.begin(), weights.end(), 1.0);
        }

        std::vector<double> scaled(entryCount);
        std::vector<size_t> smallBuckets;
        std::vector<size_t> largeBuckets;
        smallBuckets.reserve(entryCount);
        largeBuckets.reserve(entryCount);
        for (size_t index = 0; index < entryCount; ++index)
        {
            const double probability = weights[index] / totalWeight;
            entries[index].texelProbability = static_cast<float>(probability);
            entries[index].aliasIndex = static_cast<float>(index);
            scaled[index] = probability * static_cast<double>(entryCount);
            (scaled[index] < 1.0 ? smallBuckets : largeBuckets).push_back(index);
        }

        while (!smallBuckets.empty() && !largeBuckets.empty())
        {
            const size_t smallIndex = smallBuckets.back();
            smallBuckets.pop_back();
            const size_t largeIndex = largeBuckets.back();
            largeBuckets.pop_back();
            entries[smallIndex].acceptProbability = static_cast<float>(std::clamp(scaled[smallIndex], 0.0, 1.0));
            entries[smallIndex].aliasIndex = static_cast<float>(largeIndex);
            scaled[largeIndex] = (scaled[largeIndex] + scaled[smallIndex]) - 1.0;
            (scaled[largeIndex] < 1.0 ? smallBuckets : largeBuckets).push_back(largeIndex);
        }
        for (size_t index : largeBuckets)
        {
            entries[index].acceptProbability = 1.0f;
            entries[index].aliasIndex = static_cast<float>(index);
        }
        for (size_t index : smallBuckets)
        {
            entries[index].acceptProbability = 1.0f;
            entries[index].aliasIndex = static_cast<float>(index);
        }

        // The shader samples the float32 acceptance table. Reconstruct its
        // realized PMF after quantization so the PDF used by MIS exactly
        // matches the discrete distribution that generated the direction.
        std::vector<double> realizedProbability(entryCount, 0.0);
        const double bucketProbability = 1.0 / static_cast<double>(entryCount);
        for (size_t index = 0; index < entryCount; ++index)
        {
            const double accept = static_cast<double>(entries[index].acceptProbability);
            const size_t aliasIndex = std::min(
                static_cast<size_t>(std::llround(entries[index].aliasIndex)),
                entryCount - 1);
            realizedProbability[index] += bucketProbability * accept;
            realizedProbability[aliasIndex] += bucketProbability * (1.0 - accept);
        }
        for (size_t index = 0; index < entryCount; ++index)
        {
            entries[index].texelProbability = static_cast<float>(realizedProbability[index]);
        }
        return entries;
    }
}

#include "BasisKtx2Loader.h"

#include <basisu_transcoder.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <vector>

namespace
{
constexpr std::array<std::uint8_t, 12> Ktx2Identifier = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
constexpr std::uint64_t MaxKtx2Bytes = 1024ull * 1024ull * 1024ull;

std::uint32_t ReadU32(const std::uint8_t* value)
{
    std::uint32_t result;
    std::memcpy(&result, value, sizeof(result));
    return result;
}

std::uint64_t ReadU64(const std::uint8_t* value)
{
    std::uint64_t result;
    std::memcpy(&result, value, sizeof(result));
    return result;
}

bool ReadFile(const std::wstring& path, std::vector<std::uint8_t>& bytes)
{
    std::error_code error;
    const std::uint64_t size = std::filesystem::file_size(path, error);
    if (error || size < 80 || size > MaxKtx2Bytes || size > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) return false;
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    return input && static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
}

struct NativeFormat
{
    DXGI_FORMAT linear = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srgb = DXGI_FORMAT_UNKNOWN;
    std::uint32_t blockWidth = 1;
    std::uint32_t blockHeight = 1;
    std::uint32_t blockBytes = 0;
};

NativeFormat MapVkFormat(std::uint32_t format)
{
    switch (format)
    {
    case 37: return { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, 1, 4 };
    case 43: return { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, 1, 4 };
    case 131: case 133: return { DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, 4, 4, 8 };
    case 132: case 134: return { DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, 4, 4, 8 };
    case 135: return { DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB, 4, 4, 16 };
    case 136: return { DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM_SRGB, 4, 4, 16 };
    case 137: return { DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB, 4, 4, 16 };
    case 138: return { DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM_SRGB, 4, 4, 16 };
    case 139: return { DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, 4, 4, 8 };
    case 140: return { DXGI_FORMAT_BC4_SNORM, DXGI_FORMAT_BC4_SNORM, 4, 4, 8 };
    case 141: return { DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, 4, 4, 16 };
    case 142: return { DXGI_FORMAT_BC5_SNORM, DXGI_FORMAT_BC5_SNORM, 4, 4, 16 };
    case 143: return { DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_UF16, 4, 4, 16 };
    case 144: return { DXGI_FORMAT_BC6H_SF16, DXGI_FORMAT_BC6H_SF16, 4, 4, 16 };
    case 145: case 146: return { DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB, 4, 4, 16 };
    default: return {};
    }
}

std::uint32_t FirstResidentLevel(std::uint32_t width, std::uint32_t height, std::uint32_t levels, std::uint32_t maxDimension)
{
    std::uint32_t level = 0;
    const std::uint32_t limit = maxDimension == 0 ? 1u : maxDimension;
    while (level + 1 < levels && std::max(width >> level, height >> level) > limit) ++level;
    return level;
}

bool LoadNative(
    const std::vector<std::uint8_t>& bytes,
    bool srgb,
    std::uint32_t maxDimension,
    Bistro::TextureData& texture,
    std::string& diagnostics)
{
    const std::uint32_t vkFormat = ReadU32(bytes.data() + 12);
    if (vkFormat == 0) return false;
    const NativeFormat format = MapVkFormat(vkFormat);
    if (format.linear == DXGI_FORMAT_UNKNOWN)
    {
        diagnostics = "KTX2 uses a native VkFormat that has no D3D12 mapping.";
        return false;
    }
    const std::uint32_t width = ReadU32(bytes.data() + 20);
    const std::uint32_t height = ReadU32(bytes.data() + 24);
    const std::uint32_t depth = ReadU32(bytes.data() + 28);
    const std::uint32_t layers = ReadU32(bytes.data() + 32);
    const std::uint32_t faces = ReadU32(bytes.data() + 36);
    const std::uint32_t levels = std::max(ReadU32(bytes.data() + 40), 1u);
    const std::uint32_t supercompression = ReadU32(bytes.data() + 44);
    if (width == 0 || height == 0 || depth > 1 || layers > 1 || faces != 1 || levels > 32 || supercompression != 0)
    {
        diagnostics = "Only non-array, non-cube, native KTX2 textures without supercompression are supported.";
        return false;
    }
    if (80ull + static_cast<std::uint64_t>(levels) * 24ull > bytes.size())
    {
        diagnostics = "KTX2 level index is truncated.";
        return false;
    }
    const std::uint32_t firstLevel = FirstResidentLevel(width, height, levels, maxDimension);
    texture = {};
    texture.sourceWidth = width;
    texture.sourceHeight = height;
    texture.width = std::max(width >> firstLevel, 1u);
    texture.height = std::max(height >> firstLevel, 1u);
    texture.mipLevels = levels - firstLevel;
    texture.format = srgb ? format.srgb : format.linear;
    texture.container = "ktx2-native";
    texture.transcodeFormat = "direct-upload";
    for (std::uint32_t level = firstLevel; level < levels; ++level)
    {
        const std::uint8_t* entry = bytes.data() + 80 + static_cast<std::size_t>(level) * 24;
        const std::uint64_t offset = ReadU64(entry);
        const std::uint64_t length = ReadU64(entry + 8);
        const std::uint32_t mipWidth = std::max(width >> level, 1u);
        const std::uint32_t mipHeight = std::max(height >> level, 1u);
        const std::size_t rowPitch = static_cast<std::size_t>((mipWidth + format.blockWidth - 1) / format.blockWidth) * format.blockBytes;
        const std::size_t slicePitch = rowPitch * ((mipHeight + format.blockHeight - 1) / format.blockHeight);
        if (length < slicePitch || offset > bytes.size() || length > bytes.size() - static_cast<std::size_t>(offset))
        {
            diagnostics = "KTX2 mip payload is truncated.";
            return false;
        }
        const std::size_t destination = texture.pixels.size();
        texture.pixels.insert(texture.pixels.end(), bytes.begin() + static_cast<std::size_t>(offset), bytes.begin() + static_cast<std::size_t>(offset) + slicePitch);
        texture.mips.push_back({ mipWidth, mipHeight, destination, rowPitch, slicePitch });
    }
    texture.residentBytes = texture.pixels.size();
    diagnostics = "Native KTX2 mip chain retained for direct D3D12 upload.";
    return true;
}

bool LoadBasis(
    const std::vector<std::uint8_t>& bytes,
    bool srgb,
    std::uint32_t maxDimension,
    Bistro::TextureData& texture,
    std::string& diagnostics)
{
    static std::once_flag initialize;
    std::call_once(initialize, [] { basist::basisu_transcoder_init(); });
    basist::ktx2_transcoder transcoder;
    if (!transcoder.init(bytes.data(), static_cast<std::uint32_t>(bytes.size())) || transcoder.get_faces() != 1 || transcoder.get_layers() > 1)
    {
        diagnostics = "Basis Universal could not parse this KTX2 texture.";
        return false;
    }
    if (!transcoder.start_transcoding())
    {
        diagnostics = "Basis Universal could not start KTX2 transcoding.";
        return false;
    }
    const bool hdr = transcoder.is_hdr();
    const basist::transcoder_texture_format target = hdr
        ? basist::transcoder_texture_format::cTFBC6H
        : basist::transcoder_texture_format::cTFBC7_RGBA;
    const std::uint32_t levels = transcoder.get_levels();
    const std::uint32_t firstLevel = FirstResidentLevel(transcoder.get_width(), transcoder.get_height(), levels, maxDimension);
    texture = {};
    texture.sourceWidth = transcoder.get_width();
    texture.sourceHeight = transcoder.get_height();
    texture.width = std::max(transcoder.get_width() >> firstLevel, 1u);
    texture.height = std::max(transcoder.get_height() >> firstLevel, 1u);
    texture.mipLevels = levels - firstLevel;
    texture.format = hdr ? DXGI_FORMAT_BC6H_UF16 : srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    texture.container = transcoder.is_etc1s() ? "ktx2-basis-etc1s" : hdr ? "ktx2-basis-hdr" : "ktx2-basis-uastc";
    texture.transcodeFormat = hdr ? "BC6H" : "BC7";
    for (std::uint32_t level = firstLevel; level < levels; ++level)
    {
        basist::ktx2_image_level_info info{};
        if (!transcoder.get_image_level_info(info, level, 0, 0))
        {
            diagnostics = "KTX2 mip metadata is invalid.";
            return false;
        }
        const std::size_t blockCount = static_cast<std::size_t>(info.m_num_blocks_x) * info.m_num_blocks_y;
        const std::size_t offset = texture.pixels.size();
        texture.pixels.resize(offset + blockCount * 16u);
        if (!transcoder.transcode_image_level(level, 0, 0, texture.pixels.data() + offset, static_cast<std::uint32_t>(blockCount), target))
        {
            diagnostics = "Basis Universal failed to transcode a KTX2 mip.";
            return false;
        }
        const std::size_t rowPitch = static_cast<std::size_t>(info.m_num_blocks_x) * 16u;
        texture.mips.push_back({ info.m_orig_width, info.m_orig_height, offset, rowPitch, rowPitch * info.m_num_blocks_y });
    }
    texture.residentBytes = texture.pixels.size();
    diagnostics = "Basis KTX2 transcoded to " + texture.transcodeFormat + ".";
    return true;
}
}

namespace Bistro
{
bool LoadKtx2Texture(
    const std::wstring& path,
    bool srgb,
    std::uint32_t maxDimension,
    TextureData& texture,
    std::string& diagnostics)
{
    std::vector<std::uint8_t> bytes;
    if (!ReadFile(path, bytes) || !std::equal(Ktx2Identifier.begin(), Ktx2Identifier.end(), bytes.begin()))
    {
        diagnostics = "KTX2 file is missing, oversized, or has an invalid identifier.";
        return false;
    }
    if (ReadU32(bytes.data() + 12) != 0)
    {
        return LoadNative(bytes, srgb, maxDimension, texture, diagnostics);
    }
    return LoadBasis(bytes, srgb, maxDimension, texture, diagnostics);
}
}

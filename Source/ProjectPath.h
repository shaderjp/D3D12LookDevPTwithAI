#pragma once

#include <filesystem>

namespace rb
{
inline std::filesystem::path ResolveProjectAssetPath(
    const std::filesystem::path& projectFilePath,
    const std::filesystem::path& assetPath)
{
    if (assetPath.empty() || assetPath.is_absolute())
    {
        return assetPath;
    }

    return (std::filesystem::absolute(projectFilePath).parent_path() / assetPath).lexically_normal();
}
}

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

inline std::filesystem::path ResolveProjectAssetPath(
    const std::filesystem::path& projectFilePath,
    const std::filesystem::path& assetPath,
    const std::filesystem::path& assetRoot)
{
    if (assetRoot.empty())
    {
        return ResolveProjectAssetPath(projectFilePath, assetPath);
    }
    if (assetPath.empty() || assetPath.is_absolute())
    {
        return {};
    }

    const std::filesystem::path projectDirectory =
        std::filesystem::absolute(projectFilePath).parent_path();
    const std::filesystem::path root = (assetRoot.is_absolute()
        ? assetRoot
        : projectDirectory / assetRoot).lexically_normal();
    const std::filesystem::path resolved = (root / assetPath).lexically_normal();
    const std::filesystem::path relative = resolved.lexically_relative(root);
    if (relative.empty() || *relative.begin() == L"..")
    {
        return {};
    }
    return resolved;
}
}

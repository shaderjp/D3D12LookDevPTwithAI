#pragma once

#include "SceneTypes.h"

#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace rb
{
inline constexpr wchar_t SceneOpenDialogFilter[] =
    L"All Supported Scenes (*.pbrt;*.gltf;*.glb;*.fbx;*.obj)\0*.pbrt;*.gltf;*.glb;*.fbx;*.obj\0"
    L"PBRT v4 Scene (*.pbrt)\0*.pbrt\0"
    L"glTF Scene (*.gltf;*.glb)\0*.gltf;*.glb\0"
    L"FBX Scene (*.fbx)\0*.fbx\0"
    L"Wavefront OBJ (*.obj)\0*.obj\0"
    L"All Files (*.*)\0*.*\0";

inline bool SceneExtensionEquals(std::wstring_view extension, std::wstring_view expected) noexcept
{
    if (extension.size() != expected.size()) return false;
    for (std::size_t index = 0; index < extension.size(); ++index)
    {
        if (std::towlower(extension[index]) != std::towlower(expected[index])) return false;
    }
    return true;
}

inline bool IsPbrtScenePath(const std::filesystem::path& path) noexcept
{
    return SceneExtensionEquals(path.extension().wstring(), L".pbrt");
}

inline bool IsSupportedScenePath(const std::filesystem::path& path) noexcept
{
    const std::wstring extension = path.extension().wstring();
    return SceneExtensionEquals(extension, L".pbrt") ||
        SceneExtensionEquals(extension, L".gltf") ||
        SceneExtensionEquals(extension, L".glb") ||
        SceneExtensionEquals(extension, L".fbx") ||
        SceneExtensionEquals(extension, L".obj");
}

struct SceneImportResult
{
    bool succeeded = false;
    std::string diagnostics;
    ImportedScene scene;
};

class SceneImporter
{
public:
    SceneImportResult ImportScene(const std::wstring& path);
};
}

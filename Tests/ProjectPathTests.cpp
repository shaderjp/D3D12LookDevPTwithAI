#include "ProjectPath.h"

#include <filesystem>
#include <stdexcept>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}
}

int main()
{
    const std::filesystem::path workingDirectory = std::filesystem::current_path();
    const std::filesystem::path projectPath = workingDirectory / L"portable-root" / L"projects" / L"shot.lookdevpt.json";

    const std::filesystem::path relativeScene = L"..\\Bistro_v5_2\\BistroExterior.fbx";
    const std::filesystem::path expectedScene =
        (workingDirectory / L"portable-root" / L"Bistro_v5_2" / L"BistroExterior.fbx").lexically_normal();
    Require(rb::ResolveProjectAssetPath(projectPath, relativeScene) == expectedScene,
        "relative scene path was not resolved from the project directory");

    const std::filesystem::path relativeEnvironment = L"environment\\studio.hdr";
    const std::filesystem::path expectedEnvironment =
        (projectPath.parent_path() / relativeEnvironment).lexically_normal();
    Require(rb::ResolveProjectAssetPath(projectPath, relativeEnvironment) == expectedEnvironment,
        "relative environment path was not resolved from the project directory");

    const std::filesystem::path absoluteAsset =
        (workingDirectory / L"absolute-assets" / L"scene.fbx").lexically_normal();
    Require(rb::ResolveProjectAssetPath(projectPath, absoluteAsset) == absoluteAsset,
        "absolute asset path changed during project resolution");

    Require(rb::ResolveProjectAssetPath(projectPath, {}).empty(),
        "empty asset path did not remain empty");

    const std::filesystem::path bundleProject =
        workingDirectory / L"bundle" / L"project" / L"scene.lookdevpt.json";
    const std::filesystem::path bundleAsset = rb::ResolveProjectAssetPath(
        bundleProject, L"scenes\\scene.pbrt", L"..\\assets");
    Require(bundleAsset ==
        (workingDirectory / L"bundle" / L"assets" / L"scenes" / L"scene.pbrt").lexically_normal(),
        "bundle assetRoot was not resolved relative to the project file");
    Require(rb::ResolveProjectAssetPath(bundleProject, L"..\\secret.txt", L"..\\assets").empty(),
        "bundle asset traversal was not rejected");
    Require(rb::ResolveProjectAssetPath(bundleProject, absoluteAsset, L"..\\assets").empty(),
        "bundle project retained an absolute asset path");
    return 0;
}

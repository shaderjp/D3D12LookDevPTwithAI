#include "PbrtSceneImporter.h"

#include <DirectXMath.h>
#include <Windows.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void WriteText(const std::filesystem::path& path, const char* text)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not create PBRT test fixture.");
    file << text;
}

void TestTriangleMeshIncludeAndInstances(const std::filesystem::path& root)
{
    WriteText(root / "materials.pbrt",
        "MakeNamedMaterial \"paint\" \"string type\" \"diffuse\" \"rgb reflectance\" [0.2 0.4 0.8]\n");
    WriteText(root / "scene.pbrt",
        "LookAt 0 1 -5 0 1 0 0 1 0\n"
        "Camera \"perspective\" \"float fov\" 53\n"
        "Film \"rgb\" \"integer xresolution\" 640 \"integer yresolution\" 360\n"
        "WorldBegin\n"
        "Include \"materials.pbrt\"\n"
        "AttributeBegin\n"
        "NamedMaterial \"paint\"\n"
        "ObjectBegin \"panel\"\n"
        "Shape \"trianglemesh\" \"integer indices\" [0 1 2] "
        "\"point3 P\" [0 0 0 1 0 0 0 1 0] "
        "\"point2 uv\" [0 0 1 0 0 1]\n"
        "ObjectEnd\n"
        "AttributeEnd\n"
        "Translate 1 0 0\n"
        "ObjectInstance \"panel\"\n"
        "Translate 2 0 0\n"
        "ObjectInstance \"panel\"\n"
        "LightSource \"point\" \"rgb I\" [10 8 6]\n");

    const rb::SceneImportResult result = rb::ImportPbrtScene((root / "scene.pbrt").wstring());
    Require(result.succeeded, result.diagnostics.c_str());
    Require(result.scene.meshes.size() == 1, "Object instances must share one mesh.");
    Require(result.scene.instances.size() == 2, "Two ObjectInstance directives must produce two instances.");
    Require(result.scene.meshes[0].vertices.size() == 3, "trianglemesh vertex count is incorrect.");
    Require(result.scene.meshes[0].indices.size() == 3, "trianglemesh index count is incorrect.");
    Require(result.scene.materials.size() >= 2, "Named material was not created.");
    Require(result.scene.camera.has_value(), "Perspective camera was not imported.");
    Require(std::abs(result.scene.camera->fovDegrees - 53.0f) < 0.001f, "Camera FOV was not imported.");
    Require(std::abs(result.scene.camera->position.x) < 0.001f &&
        std::abs(result.scene.camera->position.y - 1.0f) < 0.001f &&
        std::abs(result.scene.camera->position.z + 5.0f) < 0.001f,
        "PBRT world-to-camera CTM was not inverted for renderer camera state.");
    Require(result.scene.lights.size() == 1, "Point light was not imported.");
    Require(result.scene.hasAuthoredLighting, "Authored lighting marker was not set.");
    Require(result.scene.boundsMax.x >= 3.0f, "Instance transforms were not included in scene bounds.");
}

void TestIncludeCycle(const std::filesystem::path& root)
{
    WriteText(root / "cycle-a.pbrt", "Include \"cycle-b.pbrt\"\n");
    WriteText(root / "cycle-b.pbrt", "Include \"cycle-a.pbrt\"\n");
    const rb::SceneImportResult result = rb::ImportPbrtScene((root / "cycle-a.pbrt").wstring());
    Require(!result.succeeded, "Include cycle must fail import.");
    Require(result.diagnostics.find("cycle") != std::string::npos, "Include-cycle diagnostic is missing.");

    for (int depth = 0; depth < 65; ++depth)
    {
        const std::string body = depth == 64
            ? "WorldBegin\n"
            : "Include \"deep-" + std::to_string(depth + 1) + ".pbrt\"\n";
        WriteText(root / ("deep-" + std::to_string(depth) + ".pbrt"), body.c_str());
    }
    const rb::SceneImportResult deepResult = rb::ImportPbrtScene((root / "deep-0.pbrt").wstring());
    Require(!deepResult.succeeded && deepResult.diagnostics.find("64") != std::string::npos,
        "Include depth limit must fail with an explicit diagnostic.");
}

void TestPlyNestedInstancesMaterialsAlphaAndLights(const std::filesystem::path& root)
{
    WriteText(root / "triangle.ply",
        "ply\nformat ascii 1.0\nelement vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
        "0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n");
    WriteText(root / "alpha.png", "fixture only; importer checks path existence\n");
    WriteText(root / "features.pbrt",
        "WorldBegin\n"
        "Texture \"alpha-map\" \"float\" \"imagemap\" \"string filename\" \"alpha.png\" "
        "\"float vscale\" -1 \"float udelta\" 0.25\n"
        "MakeNamedMaterial \"metal\" \"string type\" \"conductor\" \"spectrum eta\" \"metal-Ag-eta\" \"spectrum k\" \"metal-Ag-k\"\n"
        "MakeNamedMaterial \"paint\" \"string type\" \"coateddiffuse\" \"rgb reflectance\" [0.2 0.4 0.8] \"float roughness\" 0.08\n"
        "MakeNamedMaterial \"coated-metal\" \"string type\" \"coatedconductor\" \"spectrum conductor.eta\" \"metal-Al-eta\" \"spectrum conductor.k\" \"metal-Al-k\" \"float conductor.roughness\" 0.2 \"float interface.roughness\" 0.03\n"
        "MakeNamedMaterial \"glass\" \"string type\" \"dielectric\"\n"
        "MakeNamedMaterial \"red\" \"string type\" \"diffuse\" \"rgb reflectance\" [1 0 0]\n"
        "MakeNamedMaterial \"blue\" \"string type\" \"diffuse\" \"rgb reflectance\" [0 0 1]\n"
        "MakeNamedMaterial \"blend\" \"string type\" \"mix\" \"string materials\" [\"red\" \"blue\"] \"float amount\" 0.25\n"
        "Translate 4 0 0\nCoordinateSystem \"placed\"\nIdentity\n"
        "ObjectBegin \"leaf\"\nNamedMaterial \"metal\"\nReverseOrientation\n"
        "Shape \"plymesh\" \"string filename\" \"triangle.ply\" \"texture alpha\" \"alpha-map\"\n"
        "ObjectEnd\n"
        "ObjectBegin \"branch\"\nTranslate 1 0 0\nObjectInstance \"leaf\"\nObjectEnd\n"
        "CoordSysTransform \"placed\"\nObjectInstance \"branch\"\n"
        "AttributeBegin\nAreaLightSource \"diffuse\" \"blackbody L\" [4000] \"float scale\" 3 \"bool twosided\" true\n"
        "Shape \"trianglemesh\" \"integer indices\" [0 1 2] \"point3 P\" [0 0 0 0 1 0 0 0 1]\nAttributeEnd\n"
        "LightSource \"point\" \"rgb I\" [1 2 3]\n"
        "LightSource \"spot\" \"rgb I\" [2 3 4] \"float coneangle\" 20\n"
        "LightSource \"distant\" \"rgb L\" [3 4 5] \"point3 from\" [0 0 0] \"point3 to\" [0 0 1]\n"
        "LightSource \"infinite\" \"rgb L\" [0.25 0.5 1]\n");

    const rb::SceneImportResult result = rb::ImportPbrtScene((root / "features.pbrt").wstring());
    Require(result.succeeded, result.diagnostics.c_str());
    Require(result.scene.meshes.size() == 2, "PLY and trianglemesh must produce two shared meshes.");
    Require(result.scene.instances.size() == 2, "Nested object reference and area shape must produce two instances.");
    Require(result.scene.instances[0].transform._41 >= 5.0f, "Coordinate-system and nested-object transforms were not composed.");
    Require(result.scene.lights.size() == 3, "Point, spot, and distant lights were not imported.");
    Require(result.scene.environment.has_value(), "Infinite light was not imported.");
    Require(result.scene.hasAuthoredLighting, "PBRT lights must disable synthetic LookDev lighting.");
    bool foundMetal = false;
    bool foundAlpha = false;
    bool foundTwoSidedEmission = false;
    bool foundMaterialMix = false;
    bool foundDielectricTransmission = false;
    bool foundCoatedDiffuse = false;
    bool foundCoatedConductor = false;
    for (const rb::SceneMaterial& material : result.scene.materials)
    {
        foundMetal |= material.assignment.materialName.find("metal") != std::string::npos && material.assignment.metallicFactor == 1.0f;
        foundAlpha |= material.hasAlphaTexture && std::abs(material.assignment.alphaCutoff - 0.5f) < 1e-6f &&
            std::abs(material.uvScaleOffset.y + 1.0f) < 1e-6f &&
            std::abs(material.uvScaleOffset.z - 0.25f) < 1e-6f;
        foundTwoSidedEmission |= material.twoSidedEmission;
        foundMaterialMix |= material.assignment.materialName == "blend" &&
            std::abs(material.assignment.baseColorFactor[0] - 0.75f) < 1.0e-6f &&
            std::abs(material.assignment.baseColorFactor[2] - 0.25f) < 1.0e-6f;
        foundDielectricTransmission |= material.assignment.materialName == "glass" &&
            material.transmissionFactor > 0.99f &&
            std::abs(material.indexOfRefraction - 1.5f) < 1.0e-6f &&
            material.assignment.metallicFactor == 0.0f;
        foundCoatedDiffuse |= material.assignment.materialName == "paint" &&
            material.assignment.roughnessFactor == 1.0f &&
            (material.gltfExtensions.featureMask & rb::GltfMaterialFeatureClearcoat) != 0u &&
            material.gltfExtensions.clearcoatFactor == 1.0f &&
            std::abs(material.gltfExtensions.clearcoatRoughnessFactor - 0.08f) < 1.0e-6f;
        foundCoatedConductor |= material.assignment.materialName == "coated-metal" &&
            std::abs(material.assignment.roughnessFactor - 0.2f) < 1.0e-6f &&
            material.assignment.metallicFactor == 1.0f &&
            std::abs(material.assignment.baseColorFactor[0] - 0.91f) < 1.0e-6f &&
            (material.gltfExtensions.featureMask & rb::GltfMaterialFeatureClearcoat) != 0u &&
            material.gltfExtensions.clearcoatFactor == 1.0f &&
            std::abs(material.gltfExtensions.clearcoatRoughnessFactor - 0.03f) < 1.0e-6f;
    }
    Require(foundMetal, "Named conductor approximation is missing.");
    Require(foundAlpha, "Independent alpha texture was not imported.");
    Require(foundTwoSidedEmission, "Two-sided diffuse area light state was not imported.");
    Require(foundMaterialMix, "Constant-coefficient mix material was not evaluated.");
    Require(foundDielectricTransmission, "PBRT dielectric material was not marked for transmission.");
    Require(foundCoatedDiffuse, "PBRT coateddiffuse was not mapped to a diffuse base plus clearcoat.");
    Require(foundCoatedConductor, "PBRT coatedconductor roughness and clearcoat were not preserved.");
    Require(result.diagnostics.find("Straight-through approximation for dielectric") == std::string::npos,
        "The obsolete straight-through dielectric warning is still reported.");
}

void TestDiskAreaLight(const std::filesystem::path& root)
{
    WriteText(root / "disk.pbrt",
        "WorldBegin\nAreaLightSource \"diffuse\" \"rgb L\" [4 3 2]\n"
        "Shape \"disk\" \"float radius\" 2 \"float innerradius\" 0.5 \"float phimax\" 180\n");
    const rb::SceneImportResult result = rb::ImportPbrtScene((root / "disk.pbrt").wstring());
    Require(result.succeeded, result.diagnostics.c_str());
    Require(result.scene.meshes.size() == 1 && result.scene.instances.size() == 1,
        "PBRT disk must be tessellated into one instanced mesh.");
    Require(!result.scene.meshes[0].indices.empty() && result.scene.meshes[0].indices.size() % 3 == 0,
        "PBRT disk tessellation did not produce triangles.");
    bool foundEmission = false;
    for (const rb::SceneMaterial& material : result.scene.materials)
    {
        foundEmission |= material.emissiveFactor.x > 0.0f || material.emissiveFactor.y > 0.0f || material.emissiveFactor.z > 0.0f;
    }
    Require(foundEmission, "PBRT disk area light did not retain its emission.");
}

void TestMalformedAndMissingAssets(const std::filesystem::path& root)
{
    WriteText(root / "missing-include.pbrt", "Include \"does-not-exist.pbrt\"\n");
    rb::SceneImportResult result = rb::ImportPbrtScene((root / "missing-include.pbrt").wstring());
    Require(!result.succeeded && result.diagnostics.find("not found") != std::string::npos, "Missing Include must fail with a diagnostic.");

    WriteText(root / "missing-ply.pbrt", "WorldBegin\nShape \"plymesh\" \"string filename\" \"does-not-exist.ply\"\n");
    result = rb::ImportPbrtScene((root / "missing-ply.pbrt").wstring());
    Require(!result.succeeded && result.diagnostics.find("PLY file was not found") != std::string::npos, "Missing PLY must fail with a diagnostic.");

    WriteText(root / "bad-index.pbrt", "WorldBegin\nShape \"trianglemesh\" \"integer indices\" [0 1 3] \"point3 P\" [0 0 0 1 0 0 0 1 0]\n");
    result = rb::ImportPbrtScene((root / "bad-index.pbrt").wstring());
    Require(!result.succeeded && result.diagnostics.find("outside the vertex array") != std::string::npos, "Invalid triangle index must fail.");

    WriteText(root / "singular-transform.pbrt",
        "WorldBegin\nScale 0 1 1\n"
        "Shape \"trianglemesh\" \"integer indices\" [0 1 2] \"point3 P\" [0 0 0 1 0 0 0 1 0]\n");
    result = rb::ImportPbrtScene((root / "singular-transform.pbrt").wstring());
    Require(!result.succeeded && result.diagnostics.find("singular") != std::string::npos,
        "Singular instance transform must fail before GPU allocation.");

    WriteText(root / "object-cycle.pbrt",
        "WorldBegin\nObjectBegin \"a\"\nObjectInstance \"b\"\nObjectEnd\n"
        "ObjectBegin \"b\"\nObjectInstance \"a\"\nObjectEnd\nObjectInstance \"a\"\n");
    result = rb::ImportPbrtScene((root / "object-cycle.pbrt").wstring());
    Require(!result.succeeded && result.diagnostics.find("cycle") != std::string::npos, "ObjectInstance cycle must fail.");

    WriteText(root / "unknown.pbrt", "WorldBegin\nDefinitelyNotPbrt \"x\"\n");
    result = rb::ImportPbrtScene((root / "unknown.pbrt").wstring());
    Require(!result.succeeded && result.diagnostics.find("unknown PBRT directive") != std::string::npos, "Unknown directive must not be silently ignored.");

    WriteText(root / "texture-cycle.pbrt",
        "WorldBegin\n"
        "Texture \"a\" \"float\" \"scale\" \"texture tex\" \"a\" \"float scale\" 1\n"
        "Material \"diffuse\" \"texture roughness\" \"a\"\n"
        "Shape \"trianglemesh\" \"integer indices\" [0 1 2] \"point3 P\" [0 0 0 1 0 0 0 1 0]\n");
    result = rb::ImportPbrtScene((root / "texture-cycle.pbrt").wstring());
    Require(result.succeeded && result.diagnostics.find("cycle or excessive depth") != std::string::npos,
        "Cyclic texture graph must fall back with an aggregate warning.");

    WriteText(root / "constant-alpha.pbrt",
        "WorldBegin\nMaterial \"diffuse\"\n"
        "Shape \"trianglemesh\" \"float alpha\" 0.25 \"integer indices\" [0 1 2] "
        "\"point3 P\" [0 0 0 1 0 0 0 1 0]\n");
    result = rb::ImportPbrtScene((root / "constant-alpha.pbrt").wstring());
    bool foundConstantAlpha = false;
    for (const rb::SceneMaterial& material : result.scene.materials)
    {
        foundConstantAlpha |= material.assignment.alphaMode == rb::AlphaMode::Mask &&
            std::abs(material.assignment.baseColorFactor[3] - 0.25f) < 1.0e-6f;
    }
    Require(result.succeeded && foundConstantAlpha, "Constant PBRT shape alpha was not mapped to cutout coverage.");
}

void TestCancellation(const std::filesystem::path& root)
{
    WriteText(root / "cancel.pbrt", "WorldBegin\nShape \"trianglemesh\" \"integer indices\" [0 1 2] \"point3 P\" [0 0 0 1 0 0 0 1 0]\n");
    std::atomic_bool cancelled = true;
    const rb::SceneImportResult result = rb::ImportPbrtScene((root / "cancel.pbrt").wstring(), &cancelled);
    Require(!result.succeeded, "Pre-cancelled import must fail.");
    Require(result.diagnostics.find("cancelled") != std::string::npos, "Cancellation diagnostic is missing.");
}

void TestExternalScene(const std::filesystem::path& path)
{
    const rb::SceneImportResult result = rb::ImportPbrtScene(path.wstring());
    if (!result.succeeded)
    {
        throw std::runtime_error("Failed to import " + path.string() + ": " + result.diagnostics);
    }
    Require(!result.scene.meshes.empty(), "External PBRT scene did not produce any mesh definitions.");
    Require(!result.scene.instances.empty(), "External PBRT scene did not produce any instances.");
    const std::string filename = path.filename().string();
    if (filename == "bmw-m6.pbrt")
    {
        bool foundWindscreen = false;
        for (const rb::SceneMaterial& material : result.scene.materials)
        {
            foundWindscreen |= material.assignment.materialName == "WindscreenGlass" &&
                material.transmissionFactor > 0.99f &&
                std::abs(material.indexOfRefraction - 1.5f) < 1.0e-6f &&
                material.assignment.metallicFactor == 0.0f &&
                !material.thinDielectric;
        }
        Require(foundWindscreen, "BMW M6 WindscreenGlass was not imported as transmissive dielectric.");
    }
    if (filename == "villa-daylight.pbrt" || filename == "villa-lights-on.pbrt")
    {
        bool foundWindowGlass = false;
        bool foundVillaUvTransform = false;
        for (const rb::SceneMaterial& material : result.scene.materials)
        {
            foundWindowGlass |= material.assignment.materialName == "Verre" &&
                material.transmissionFactor > 0.99f &&
                material.thinDielectric;
            foundVillaUvTransform |= material.assignment.materialName == "grass_patch" &&
                std::abs(material.uvScaleOffset.x - 1.0f) < 1.0e-6f &&
                std::abs(material.uvScaleOffset.y + 1.0f) < 1.0e-6f;
        }
        Require(foundWindowGlass, "Villa Verre was not imported as thin dielectric glass.");
        Require(foundVillaUvTransform, "Villa imagemap UV scale/flip was not imported.");
    }
    if (filename == "villa-lights-on.pbrt")
    {
        Require(result.diagnostics.find("Unsupported shape skipped (1): disk") == std::string::npos,
            "Villa camera fill disk is still reported as unsupported.");
    }
    std::cout << "Imported " << path.filename().string() << ": "
              << result.scene.meshes.size() << " meshes, "
              << result.scene.instances.size() << " instances.\n";
}

void TestSceneExtensionRouting()
{
    Require(rb::IsPbrtScenePath(L"scene.pbrt"), "Lowercase PBRT extension was not recognized.");
    Require(rb::IsPbrtScenePath(L"scene.PBRT"), "Uppercase PBRT extension was not recognized.");
    Require(rb::IsSupportedScenePath(L"scene.pbrt"), "PBRT is missing from the supported scene extension set.");
    Require(!rb::IsSupportedScenePath(L"scene.exr"), "Environment images must not be routed to the scene importer.");
}
}

int wmain(int argc, wchar_t** argv)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"D3D12LookDevPT-PbrtImporterTests-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    try
    {
        TestTriangleMeshIncludeAndInstances(root);
        TestIncludeCycle(root);
        TestPlyNestedInstancesMaterialsAlphaAndLights(root);
        TestDiskAreaLight(root);
        TestMalformedAndMissingAssets(root);
        TestCancellation(root);
        TestSceneExtensionRouting();
        for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex)
        {
            TestExternalScene(std::filesystem::path(argv[argumentIndex]));
        }
        std::filesystem::remove_all(root);
        std::cout << "PBRT scene importer tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::filesystem::remove_all(root);
        std::cerr << exception.what() << '\n';
        return 1;
    }
}

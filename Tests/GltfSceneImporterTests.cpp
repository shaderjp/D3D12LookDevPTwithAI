#include "GltfSceneImporter.h"

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

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Could not create a glTF test fixture.");
    file << text;
}

std::string FixtureJson(const char* requiredExtension = nullptr, const char* imageUri = "data:image/png;base64,iVBORw0KGgo=")
{
    const std::string required = requiredExtension
        ? std::string(",\"extensionsRequired\":[\"") + requiredExtension + "\"]"
        : "";
    return std::string(R"json({
  "asset":{"version":"2.0"},
  "extensionsUsed":["KHR_texture_transform","KHR_materials_specular","KHR_materials_ior","KHR_materials_transmission","KHR_materials_volume","KHR_materials_clearcoat"])json") + required + R"json(,
  "buffers":[{"byteLength":90,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AACAPgAAgD4AAEA/AACAPgAAgD4AAEA/AAABAAIA"}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36},
    {"buffer":0,"byteOffset":36,"byteLength":24},
    {"buffer":0,"byteOffset":60,"byteLength":24},
    {"buffer":0,"byteOffset":84,"byteLength":6}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
    {"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
    {"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},
    {"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}
  ],
  "images":[{"uri":")json" + imageUri + R"json(","mimeType":"image/png"}],
  "samplers":[{"magFilter":9728,"minFilter":9728,"wrapS":33648,"wrapT":33648}],
  "textures":[{"source":0,"sampler":0}],
  "materials":[{
    "name":"Extension Material",
    "pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":0,"extensions":{"KHR_texture_transform":{"offset":[0.1,0.2],"scale":[2,3],"rotation":0.5,"texCoord":1}}}},
    "extensions":{
      "KHR_materials_specular":{"specularFactor":0.7,"specularColorFactor":[0.8,0.9,1.0]},
      "KHR_materials_ior":{"ior":1.33},
      "KHR_materials_transmission":{"transmissionFactor":0.6},
      "KHR_materials_volume":{"thicknessFactor":0.4,"attenuationDistance":2.5,"attenuationColor":[0.7,0.8,0.9]},
      "KHR_materials_clearcoat":{"clearcoatFactor":0.5,"clearcoatRoughnessFactor":0.2}
    }
  }],
  "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":2},"indices":3,"material":0}]}],
  "nodes":[{"mesh":0,"translation":[1,2,3]}],
  "scenes":[{"nodes":[0]}],"scene":0
})json";
}

void TestMaterialExtensions(const std::filesystem::path& root)
{
    const std::filesystem::path path = root / "extensions.gltf";
    WriteText(path, FixtureJson());
    const rb::SceneImportResult result = rb::ImportGltfScene(path.wstring());
    Require(result.succeeded, result.diagnostics.c_str());
    Require(result.scene.materials.size() == 1, "glTF material count is incorrect.");
    Require(result.scene.vertices.size() == 3 && result.scene.indices.size() == 3, "glTF triangle geometry was not imported.");
    const rb::SceneMaterial& material = result.scene.materials[0];
    Require(material.sourceMaterialId == "gltf:material/0", "Stable sourceMaterialId was not assigned.");
    Require(std::abs(material.gltfExtensions.ior - 1.33f) < 1.0e-4f, "KHR_materials_ior was not imported.");
    Require(std::abs(material.gltfExtensions.transmissionFactor - 0.6f) < 1.0e-4f, "KHR_materials_transmission was not imported.");
    Require(std::abs(material.gltfExtensions.thicknessFactor - 0.4f) < 1.0e-4f, "KHR_materials_volume was not imported.");
    Require(std::abs(material.gltfExtensions.clearcoatFactor - 0.5f) < 1.0e-4f, "KHR_materials_clearcoat was not imported.");
    const rb::TextureBinding& binding = material.textureBindings[static_cast<std::size_t>(rb::TextureSlot::BaseColor)];
    Require(binding.transform.texCoord == 1u, "KHR_texture_transform texCoord override was not imported.");
    Require(std::abs(binding.transform.rotation - 0.5f) < 1.0e-4f, "KHR_texture_transform rotation was not imported.");
    Require(binding.sampler == rb::TextureSamplerPreset::NearestMirror, "glTF nearest/mirror sampler was not mapped.");
    Require(std::abs(result.scene.vertices[0].texcoord1.x - 0.25f) < 1.0e-4f, "TEXCOORD_1 was not imported.");
    Require(result.scene.materialFeatureMask != 0u, "Scene material feature mask is empty.");
    Require(std::filesystem::exists(binding.path), "Embedded data URI image was not materialized.");
}

void TestRequiredExtensionFailure(const std::filesystem::path& root)
{
    const std::filesystem::path path = root / "unsupported-required.gltf";
    WriteText(path, FixtureJson("EXT_not_supported"));
    const rb::SceneImportResult result = rb::ImportGltfScene(path.wstring());
    Require(!result.succeeded, "An unsupported required extension must stop import.");
    Require(result.diagnostics.find("required") != std::string::npos, "Required-extension diagnostic is missing.");
}

void TestHttpImageFailure(const std::filesystem::path& root)
{
    const std::filesystem::path path = root / "http-image.gltf";
    WriteText(path, FixtureJson(nullptr, "https://example.invalid/texture.png"));
    const rb::SceneImportResult result = rb::ImportGltfScene(path.wstring());
    Require(!result.succeeded, "HTTP glTF images must not be fetched.");
    Require(result.diagnostics.find("HTTP") != std::string::npos, "HTTP image diagnostic is missing.");
}
}

int wmain()
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"D3D12LookDevPT-GltfImporterTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    try
    {
        TestMaterialExtensions(root);
        TestRequiredExtensionFailure(root);
        TestHttpImageFailure(root);
        std::filesystem::remove_all(root, error);
        std::cout << "GltfSceneImporterTests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::filesystem::remove_all(root, error);
        std::cerr << "GltfSceneImporterTests failed: " << exception.what() << '\n';
        return 1;
    }
}

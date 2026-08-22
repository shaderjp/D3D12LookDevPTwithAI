#pragma once

#include "QualitySettings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rb
{
enum class TextureSlot : std::uint32_t
{
    BaseColor,
    Normal,
    Roughness,
    Metallic,
    Occlusion,
    Emissive,
    Alpha,
    SpecularColor,
    SpecularFactor,
    Transmission,
    Thickness,
    Clearcoat,
    ClearcoatRoughness,
    ClearcoatNormal,
    Count
};
static_assert(static_cast<std::uint32_t>(TextureSlot::BaseColor) == 0u);
static_assert(static_cast<std::uint32_t>(TextureSlot::Emissive) == 5u,
    "Existing project texture-slot values must remain stable.");
static_assert(static_cast<std::uint32_t>(TextureSlot::Alpha) == 6u,
    "Alpha must remain the appended seventh texture slot.");
static_assert(static_cast<std::uint32_t>(TextureSlot::SpecularColor) == 7u,
    "glTF extension texture slots must only be appended.");
static_assert(static_cast<std::uint32_t>(TextureSlot::ClearcoatNormal) == 13u,
    "The glTF material extension texture ABI changed.");

enum class TextureSamplerPreset : std::uint32_t
{
    LinearRepeat,
    LinearClamp,
    LinearMirror,
    NearestRepeat,
    NearestClamp,
    NearestMirror
};

enum class TextureResolutionPolicy : std::uint32_t
{
    Auto,
    Source,
    Max4096,
    Max2048,
    Max1024,
    Max512
};

struct TextureTransform
{
    std::array<float, 2> offset = { 0.0f, 0.0f };
    std::array<float, 2> scale = { 1.0f, 1.0f };
    float rotation = 0.0f;
    std::uint32_t texCoord = 0;
};

struct TextureBinding
{
    std::wstring path;
    TextureTransform transform;
    TextureSamplerPreset sampler = TextureSamplerPreset::LinearRepeat;
    TextureResolutionPolicy resolutionPolicy = TextureResolutionPolicy::Auto;
};

enum GltfMaterialFeature : std::uint32_t
{
    GltfMaterialFeatureTextureTransform = 1u << 0,
    GltfMaterialFeatureSpecular = 1u << 1,
    GltfMaterialFeatureIor = 1u << 2,
    GltfMaterialFeatureTransmission = 1u << 3,
    GltfMaterialFeatureVolume = 1u << 4,
    GltfMaterialFeatureClearcoat = 1u << 5,
    GltfMaterialFeatureBasisu = 1u << 6,
};

struct GltfMaterialExtensions
{
    std::uint32_t featureMask = 0;
    float specularFactor = 1.0f;
    std::array<float, 3> specularColorFactor = { 1.0f, 1.0f, 1.0f };
    float ior = 1.5f;
    float transmissionFactor = 0.0f;
    float thicknessFactor = 0.0f;
    std::array<float, 3> attenuationColor = { 1.0f, 1.0f, 1.0f };
    float attenuationDistance = 3.402823466e+38F;
    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    float clearcoatNormalScale = 1.0f;
};

enum class AlphaMode : std::uint32_t
{
    Opaque,
    Mask,
    Blend
};

enum class LookDevBackgroundMode : std::uint32_t
{
    SkyColor,
    Hdri,
    TransparentChecker
};

enum class ToneMapper : std::uint32_t
{
    None,
    Reinhard,
    Aces
};

enum class LookDevDisplayMode : std::uint32_t
{
    Beauty,
    BaseColor,
    Normal,
    Roughness,
    Metallic,
    AmbientOcclusion,
    Emissive,
    LightingOnly,
    ShadowMask
};

struct MaterialAssignment
{
    std::string materialName = "Default Material";
    std::string shaderSetName = "LookDev PBR";
    std::array<std::wstring, static_cast<std::size_t>(TextureSlot::Count)> textureOverrides;
    std::array<bool, static_cast<std::size_t>(TextureSlot::Count)> textureOverrideEnabled = {};
    std::array<float, 4> baseColorFactor = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<float, 4> emissiveFactor = { 0.0f, 0.0f, 0.0f, 1.0f };
    float roughnessFactor = 0.48f;
    float metallicFactor = 0.0f;
    float normalStrength = 1.0f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.5f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    bool packedOcclusionRoughnessMetallic = false;
    bool flipNormalGreen = false;
    GltfMaterialExtensions gltfExtensions;
    std::array<TextureBinding, static_cast<std::size_t>(TextureSlot::Count)> textureBindings;
    std::array<bool, static_cast<std::size_t>(TextureSlot::Count)> textureBindingOverrideEnabled = {};
};

struct ViewportCamera
{
    std::array<float, 3> target = { 0.0f, 0.0f, 0.0f };
    float yaw = 0.0f;
    float pitch = 0.12f;
    float distance = 4.0f;
    float fovDegrees = 45.0f;
};

struct CameraBookmark
{
    bool valid = false;
    ViewportCamera camera;
};

constexpr std::size_t CameraBookmarkCount = 3;

struct ModelTransform
{
    std::array<float, 3> translation = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> rotationDegrees = { 0.0f, 0.0f, 0.0f };
};

struct LookDevEnvironment
{
    std::wstring environmentPath;
    float rotationYaw = 0.0f;
    float intensity = 1.0f;
    LookDevBackgroundMode backgroundMode = LookDevBackgroundMode::SkyColor;
    std::array<float, 3> sunDirection = { -0.35f, -0.75f, 0.55f };
    std::array<float, 3> sunColor = { 1.0f, 0.96f, 0.88f };
    float sunIntensity = 10000.0f;
};

struct LookDevViewSettings
{
    float exposure = 0.0f;
    ToneMapper toneMapper = ToneMapper::Aces;
    float gamma = 2.2f;
    LookDevDisplayMode displayMode = LookDevDisplayMode::Beauty;
    bool turntableEnabled = false;
    float turntableSpeed = 0.35f;
};

// v1.2 uses one orthographic Sun shadow map. Softness is a PCF radius in
// shadow texels, while fitScale expands the fitted scene bounds.
struct LookDevShadowSettings
{
    bool enabled = true;
    std::uint32_t resolution = 2048;
    float strength = 0.85f;
    float bias = 0.0015f;
    float softness = 1.5f;
    float fitScale = 1.25f;
};

struct UiSettings
{
    float uiFontSize = 18.0f;
    float chatFontSize = 22.0f;
    float uiScale = 1.0f;
    float chatTranscriptHeightRatio = 0.50f;
    bool largeFramePadding = true;
};

struct ProjectFile
{
    std::wstring path;
    std::wstring scenePath;
    std::array<float, 4> skyTopColor = { 0.12f, 0.22f, 0.36f, 1.0f };
    std::array<float, 4> skyHorizonColor = { 0.035f, 0.045f, 0.055f, 1.0f };
    ModelTransform modelTransform;
    ViewportCamera viewportCamera;
    std::array<CameraBookmark, CameraBookmarkCount> cameraBookmarks;
    bool hasViewportCamera = false;
    LookDevEnvironment lookDevEnvironment;
    LookDevViewSettings lookDevViewSettings;
    LookDevShadowSettings lookDevShadowSettings;
    QualitySettings qualitySettings;
    std::vector<MaterialAssignment> materialAssignments;
};
}

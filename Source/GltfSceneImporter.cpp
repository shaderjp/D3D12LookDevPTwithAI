#include "GltfSceneImporter.h"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

using namespace DirectX;

namespace
{
constexpr std::size_t MaxEmbeddedImageBytes = 512ull * 1024ull * 1024ull;

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        std::string fallback;
        fallback.reserve(text.size());
        for (wchar_t value : text) fallback.push_back(value <= 0x7f ? static_cast<char>(value) : '?');
        return fallback;
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

bool PreserveEncodedImage(
    tinygltf::Image* image,
    int,
    std::string* error,
    std::string*,
    int,
    int,
    const unsigned char* bytes,
    int size,
    void*)
{
    if (!image || !bytes || size <= 0 || static_cast<std::size_t>(size) > MaxEmbeddedImageBytes)
    {
        if (error) *error += "glTF image is empty or exceeds the 512 MiB encoded-image limit.\n";
        return false;
    }
    image->image.assign(bytes, bytes + size);
    image->as_is = true;
    return true;
}

std::string Sha256Hex(const std::vector<unsigned char>& data)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD copied = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copied, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes), &copied, 0) < 0)
    {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("Could not initialize SHA-256 for embedded glTF assets.");
    }
    std::vector<UCHAR> object(objectBytes);
    std::vector<UCHAR> digest(hashBytes);
    NTSTATUS status = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0);
    if (status >= 0 && !data.empty())
    {
        status = BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0);
    }
    if (status >= 0) status = BCryptFinishHash(hash, digest.data(), hashBytes, 0);
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Could not hash embedded glTF asset.");
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (UCHAR byte : digest) out << std::setw(2) << static_cast<unsigned int>(byte);
    return out.str();
}

std::filesystem::path CacheRoot()
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)) || !localAppData)
    {
        throw std::runtime_error("LocalAppData is unavailable for the glTF image cache.");
    }
    std::filesystem::path root(localAppData);
    CoTaskMemFree(localAppData);
    root /= L"D3D12LookDevPT";
    root /= L"GltfCache";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) throw std::runtime_error("Could not create the glTF image cache.");
    return root;
}

std::wstring ImageExtension(const tinygltf::Image& image)
{
    if (image.mimeType == "image/png") return L".png";
    if (image.mimeType == "image/jpeg") return L".jpg";
    if (image.mimeType == "image/webp") return L".webp";
    if (image.mimeType == "image/ktx2") return L".ktx2";
    const std::filesystem::path uri(Utf8ToWide(image.uri));
    if (!uri.extension().empty() && uri.extension().wstring().size() <= 12) return uri.extension().wstring();
    return L".bin";
}

bool IsHttpUri(const std::string& uri)
{
    std::string lower = uri;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

bool IsDataUri(const std::string& uri)
{
    return uri.rfind("data:", 0) == 0;
}

std::wstring MaterializeImage(const tinygltf::Image& image, const std::filesystem::path& sceneDirectory)
{
    if (!image.uri.empty() && !IsDataUri(image.uri))
    {
        if (IsHttpUri(image.uri)) throw std::runtime_error("HTTP image URIs are not supported.");
        std::filesystem::path authored(Utf8ToWide(image.uri));
        if (authored.is_absolute()) throw std::runtime_error("Absolute glTF image URIs are not supported.");
        std::error_code ec;
        const std::filesystem::path resolved = std::filesystem::weakly_canonical(sceneDirectory / authored, ec);
        const std::filesystem::path root = std::filesystem::weakly_canonical(sceneDirectory, ec);
        const std::filesystem::path relative = resolved.lexically_relative(root);
        if (ec || relative.empty() || relative.is_absolute() ||
            (!relative.empty() && *relative.begin() == L".."))
        {
            throw std::runtime_error("glTF image URI escapes the scene directory.");
        }
        if (std::filesystem::exists(resolved, ec) && !ec) return resolved.wstring();
    }
    if (image.image.empty()) return {};
    const std::string hash = Sha256Hex(image.image);
    const std::filesystem::path path = CacheRoot() / (Utf8ToWide(hash) + ImageExtension(image));
    if (!std::filesystem::exists(path))
    {
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Could not write embedded glTF image cache entry.");
            output.write(reinterpret_cast<const char*>(image.image.data()), static_cast<std::streamsize>(image.image.size()));
            if (!output) throw std::runtime_error("Embedded glTF image cache write failed.");
        }
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if (ec && !std::filesystem::exists(path)) throw std::runtime_error("Could not finalize embedded glTF image cache entry.");
        if (ec) std::filesystem::remove(temporary, ec);
    }
    return path.wstring();
}

const tinygltf::Value* Member(const tinygltf::Value& value, const char* name)
{
    return value.IsObject() && value.Has(name) ? &value.Get(name) : nullptr;
}

double Number(const tinygltf::Value& value, const char* name, double fallback)
{
    const tinygltf::Value* member = Member(value, name);
    return member && member->IsNumber() ? member->GetNumberAsDouble() : fallback;
}

int Integer(const tinygltf::Value& value, const char* name, int fallback)
{
    const tinygltf::Value* member = Member(value, name);
    return member && member->IsNumber() ? member->GetNumberAsInt() : fallback;
}

std::array<float, 3> Vec3(const tinygltf::Value& value, const char* name, std::array<float, 3> fallback)
{
    const tinygltf::Value* member = Member(value, name);
    if (!member || !member->IsArray() || member->ArrayLen() < 3) return fallback;
    for (std::size_t i = 0; i < 3; ++i)
    {
        const tinygltf::Value& component = member->Get(static_cast<int>(i));
        if (!component.IsNumber()) return fallback;
        fallback[i] = static_cast<float>(component.GetNumberAsDouble());
    }
    return fallback;
}

struct ParsedTextureInfo
{
    int textureIndex = -1;
    rb::TextureTransform transform;
};

void ApplyTextureTransform(const tinygltf::ExtensionMap& extensions, ParsedTextureInfo& info)
{
    const auto found = extensions.find("KHR_texture_transform");
    if (found == extensions.end() || !found->second.IsObject()) return;
    const tinygltf::Value& transform = found->second;
    if (const tinygltf::Value* offset = Member(transform, "offset"); offset && offset->IsArray() && offset->ArrayLen() >= 2)
    {
        info.transform.offset[0] = static_cast<float>(offset->Get(0).GetNumberAsDouble());
        info.transform.offset[1] = static_cast<float>(offset->Get(1).GetNumberAsDouble());
    }
    if (const tinygltf::Value* scale = Member(transform, "scale"); scale && scale->IsArray() && scale->ArrayLen() >= 2)
    {
        info.transform.scale[0] = static_cast<float>(scale->Get(0).GetNumberAsDouble());
        info.transform.scale[1] = static_cast<float>(scale->Get(1).GetNumberAsDouble());
    }
    info.transform.rotation = static_cast<float>(Number(transform, "rotation", 0.0));
    info.transform.texCoord = static_cast<std::uint32_t>(std::clamp(Integer(transform, "texCoord", static_cast<int>(info.transform.texCoord)), 0, 1));
}

ParsedTextureInfo ParseTextureInfo(const tinygltf::TextureInfo& source)
{
    ParsedTextureInfo result;
    result.textureIndex = source.index;
    result.transform.texCoord = static_cast<std::uint32_t>(std::clamp(source.texCoord, 0, 1));
    ApplyTextureTransform(source.extensions, result);
    return result;
}

ParsedTextureInfo ParseTextureInfo(const tinygltf::NormalTextureInfo& source)
{
    ParsedTextureInfo result;
    result.textureIndex = source.index;
    result.transform.texCoord = static_cast<std::uint32_t>(std::clamp(source.texCoord, 0, 1));
    ApplyTextureTransform(source.extensions, result);
    return result;
}

ParsedTextureInfo ParseTextureInfo(const tinygltf::OcclusionTextureInfo& source)
{
    ParsedTextureInfo result;
    result.textureIndex = source.index;
    result.transform.texCoord = static_cast<std::uint32_t>(std::clamp(source.texCoord, 0, 1));
    ApplyTextureTransform(source.extensions, result);
    return result;
}

ParsedTextureInfo ParseTextureValue(const tinygltf::Value& source)
{
    ParsedTextureInfo result;
    result.textureIndex = Integer(source, "index", -1);
    result.transform.texCoord = static_cast<std::uint32_t>(std::clamp(Integer(source, "texCoord", 0), 0, 1));
    if (const tinygltf::Value* transform = Member(source, "extensions"); transform && transform->IsObject() && transform->Has("KHR_texture_transform"))
    {
        tinygltf::ExtensionMap map;
        map["KHR_texture_transform"] = transform->Get("KHR_texture_transform");
        ApplyTextureTransform(map, result);
    }
    return result;
}

rb::TextureSamplerPreset SamplerPreset(const tinygltf::Model& model, int textureIndex)
{
    if (textureIndex < 0 || static_cast<std::size_t>(textureIndex) >= model.textures.size()) return rb::TextureSamplerPreset::LinearRepeat;
    const int samplerIndex = model.textures[textureIndex].sampler;
    if (samplerIndex < 0 || static_cast<std::size_t>(samplerIndex) >= model.samplers.size()) return rb::TextureSamplerPreset::LinearRepeat;
    const tinygltf::Sampler& sampler = model.samplers[samplerIndex];
    const bool nearest = sampler.magFilter == TINYGLTF_TEXTURE_FILTER_NEAREST;
    const bool clamp = sampler.wrapS == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE || sampler.wrapT == TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
    const bool mirror = sampler.wrapS == TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT || sampler.wrapT == TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT;
    if (nearest) return clamp ? rb::TextureSamplerPreset::NearestClamp : mirror ? rb::TextureSamplerPreset::NearestMirror : rb::TextureSamplerPreset::NearestRepeat;
    return clamp ? rb::TextureSamplerPreset::LinearClamp : mirror ? rb::TextureSamplerPreset::LinearMirror : rb::TextureSamplerPreset::LinearRepeat;
}

int TextureSourceIndex(const tinygltf::Model& model, int textureIndex, bool& basisu)
{
    basisu = false;
    if (textureIndex < 0 || static_cast<std::size_t>(textureIndex) >= model.textures.size()) return -1;
    const tinygltf::Texture& texture = model.textures[textureIndex];
    const auto extension = texture.extensions.find("KHR_texture_basisu");
    if (extension != texture.extensions.end())
    {
        basisu = true;
        return Integer(extension->second, "source", -1);
    }
    return texture.source;
}

void AssignTexture(
    rb::SceneMaterial& material,
    rb::TextureSlot slot,
    const ParsedTextureInfo& info,
    const tinygltf::Model& model,
    const std::filesystem::path& sceneDirectory,
    std::uint32_t& featureMask)
{
    if (info.textureIndex < 0) return;
    bool basisu = false;
    const int imageIndex = TextureSourceIndex(model, info.textureIndex, basisu);
    if (imageIndex < 0 || static_cast<std::size_t>(imageIndex) >= model.images.size()) return;
    rb::TextureBinding binding;
    binding.path = MaterializeImage(model.images[imageIndex], sceneDirectory);
    binding.transform = info.transform;
    binding.sampler = SamplerPreset(model, info.textureIndex);
    const std::size_t index = static_cast<std::size_t>(slot);
    material.textureBindings[index] = binding;
    if (basisu) featureMask |= rb::GltfMaterialFeatureBasisu;
    if (binding.transform.texCoord != 0 || binding.transform.rotation != 0.0f ||
        binding.transform.offset != std::array<float, 2>{ 0.0f, 0.0f } ||
        binding.transform.scale != std::array<float, 2>{ 1.0f, 1.0f })
    {
        featureMask |= rb::GltfMaterialFeatureTextureTransform;
    }
}

std::size_t ComponentCount(int type)
{
    switch (type)
    {
    case TINYGLTF_TYPE_SCALAR: return 1;
    case TINYGLTF_TYPE_VEC2: return 2;
    case TINYGLTF_TYPE_VEC3: return 3;
    case TINYGLTF_TYPE_VEC4: return 4;
    default: return 0;
    }
}

double ReadComponent(const unsigned char* data, int componentType, bool normalized)
{
    switch (componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
    {
        const auto value = *reinterpret_cast<const std::int8_t*>(data);
        return normalized ? std::max(static_cast<double>(value) / 127.0, -1.0) : value;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
    {
        const auto value = *reinterpret_cast<const std::uint8_t*>(data);
        return normalized ? static_cast<double>(value) / 255.0 : value;
    }
    case TINYGLTF_COMPONENT_TYPE_SHORT:
    {
        std::int16_t value; std::memcpy(&value, data, sizeof(value));
        return normalized ? std::max(static_cast<double>(value) / 32767.0, -1.0) : value;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
    {
        std::uint16_t value; std::memcpy(&value, data, sizeof(value));
        return normalized ? static_cast<double>(value) / 65535.0 : value;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    {
        std::uint32_t value; std::memcpy(&value, data, sizeof(value)); return value;
    }
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
    {
        float value; std::memcpy(&value, data, sizeof(value)); return value;
    }
    case TINYGLTF_COMPONENT_TYPE_DOUBLE:
    {
        double value; std::memcpy(&value, data, sizeof(value)); return value;
    }
    default: throw std::runtime_error("Unsupported glTF accessor component type.");
    }
}

class AccessorView
{
public:
    AccessorView(const tinygltf::Model& model, int accessorIndex)
    {
        if (accessorIndex < 0 || static_cast<std::size_t>(accessorIndex) >= model.accessors.size()) throw std::runtime_error("glTF accessor index is out of range.");
        m_accessor = &model.accessors[accessorIndex];
        m_components = ComponentCount(m_accessor->type);
        const int componentBytes = tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(m_accessor->componentType));
        if (m_components == 0 || componentBytes <= 0) throw std::runtime_error("Unsupported glTF accessor shape.");
        m_componentBytes = static_cast<std::size_t>(componentBytes);
        const std::size_t elementBytes = m_components * m_componentBytes;
        if (m_accessor->bufferView >= 0)
        {
            if (static_cast<std::size_t>(m_accessor->bufferView) >= model.bufferViews.size()) throw std::runtime_error("glTF accessor has no valid bufferView.");
            const tinygltf::BufferView& view = model.bufferViews[m_accessor->bufferView];
            if (view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= model.buffers.size()) throw std::runtime_error("glTF bufferView has no valid buffer.");
            const tinygltf::Buffer& buffer = model.buffers[view.buffer];
            const int byteStride = m_accessor->ByteStride(view);
            if (byteStride < 0) throw std::runtime_error("Invalid glTF accessor stride.");
            m_stride = byteStride > 0 ? static_cast<std::size_t>(byteStride) : elementBytes;
            const std::size_t offset = view.byteOffset + m_accessor->byteOffset;
            if (offset > buffer.data.size() || m_accessor->count > (buffer.data.size() - offset) / m_stride) throw std::runtime_error("glTF accessor exceeds its buffer.");
            m_data = buffer.data.data() + offset;
        }
        else if (!m_accessor->sparse.isSparse)
        {
            throw std::runtime_error("glTF accessor has no bufferView or sparse values.");
        }

        if (m_accessor->sparse.isSparse)
        {
            if (m_accessor->sparse.count < 0 || static_cast<std::size_t>(m_accessor->sparse.count) > m_accessor->count)
                throw std::runtime_error("glTF sparse accessor count is invalid.");
            m_owned.assign(m_accessor->count * elementBytes, 0u);
            if (m_data)
            {
                for (std::size_t i = 0; i < m_accessor->count; ++i)
                    std::memcpy(m_owned.data() + i * elementBytes, m_data + i * m_stride, elementBytes);
            }
            const auto viewBytes = [&](int viewIndex, std::size_t extraOffset, std::size_t required, std::size_t& stride, std::size_t tightStride) -> const unsigned char*
            {
                if (viewIndex < 0 || static_cast<std::size_t>(viewIndex) >= model.bufferViews.size()) throw std::runtime_error("glTF sparse bufferView is invalid.");
                const tinygltf::BufferView& view = model.bufferViews[viewIndex];
                if (view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= model.buffers.size()) throw std::runtime_error("glTF sparse buffer is invalid.");
                const tinygltf::Buffer& buffer = model.buffers[view.buffer];
                stride = view.byteStride > 0 ? view.byteStride : tightStride;
                const std::size_t offset = view.byteOffset + extraOffset;
                if (stride < tightStride || offset > buffer.data.size() || required > 0u && required > (buffer.data.size() - offset) / stride)
                    throw std::runtime_error("glTF sparse accessor exceeds its buffer.");
                return buffer.data.data() + offset;
            };
            const int sparseIndexType = m_accessor->sparse.indices.componentType;
            if (sparseIndexType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
                sparseIndexType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
                sparseIndexType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                throw std::runtime_error("glTF sparse index type is invalid.");
            const std::size_t sparseIndexBytes = static_cast<std::size_t>(tinygltf::GetComponentSizeInBytes(static_cast<std::uint32_t>(sparseIndexType)));
            std::size_t indexStride = 0;
            std::size_t valueStride = 0;
            const std::size_t sparseCount = static_cast<std::size_t>(m_accessor->sparse.count);
            const unsigned char* indices = viewBytes(m_accessor->sparse.indices.bufferView, m_accessor->sparse.indices.byteOffset, sparseCount, indexStride, sparseIndexBytes);
            const unsigned char* values = viewBytes(m_accessor->sparse.values.bufferView, m_accessor->sparse.values.byteOffset, sparseCount, valueStride, elementBytes);
            for (std::size_t i = 0; i < sparseCount; ++i)
            {
                const double sparseIndexValue = ReadComponent(indices + i * indexStride, sparseIndexType, false);
                if (sparseIndexValue < 0.0 || sparseIndexValue >= static_cast<double>(m_accessor->count)) throw std::runtime_error("glTF sparse index is out of range.");
                const std::size_t sparseIndex = static_cast<std::size_t>(sparseIndexValue);
                std::memcpy(m_owned.data() + sparseIndex * elementBytes, values + i * valueStride, elementBytes);
            }
            m_data = m_owned.data();
            m_stride = elementBytes;
        }
    }

    std::size_t Count() const { return m_accessor->count; }
    std::size_t Components() const { return m_components; }
    double At(std::size_t element, std::size_t component) const
    {
        if (element >= Count() || component >= m_components) throw std::runtime_error("glTF accessor element is out of range.");
        return ReadComponent(m_data + element * m_stride + component * m_componentBytes, m_accessor->componentType, m_accessor->normalized);
    }
    std::uint32_t Index(std::size_t element) const
    {
        const double value = At(element, 0);
        if (value < 0.0 || value > static_cast<double>(UINT32_MAX)) throw std::runtime_error("glTF index is out of range.");
        return static_cast<std::uint32_t>(value);
    }

private:
    const tinygltf::Accessor* m_accessor = nullptr;
    const unsigned char* m_data = nullptr;
    std::size_t m_stride = 0;
    std::size_t m_components = 0;
    std::size_t m_componentBytes = 0;
    std::vector<unsigned char> m_owned;
};

XMMATRIX NodeMatrix(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16)
    {
        return XMMatrixSet(
            static_cast<float>(node.matrix[0]), static_cast<float>(node.matrix[1]), static_cast<float>(node.matrix[2]), static_cast<float>(node.matrix[3]),
            static_cast<float>(node.matrix[4]), static_cast<float>(node.matrix[5]), static_cast<float>(node.matrix[6]), static_cast<float>(node.matrix[7]),
            static_cast<float>(node.matrix[8]), static_cast<float>(node.matrix[9]), static_cast<float>(node.matrix[10]), static_cast<float>(node.matrix[11]),
            static_cast<float>(node.matrix[12]), static_cast<float>(node.matrix[13]), static_cast<float>(node.matrix[14]), static_cast<float>(node.matrix[15]));
    }
    XMMATRIX scale = XMMatrixIdentity();
    XMMATRIX rotation = XMMatrixIdentity();
    XMMATRIX translation = XMMatrixIdentity();
    if (node.scale.size() == 3) scale = XMMatrixScaling(static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2]));
    if (node.rotation.size() == 4) rotation = XMMatrixRotationQuaternion(XMVectorSet(static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]), static_cast<float>(node.rotation[3])));
    if (node.translation.size() == 3) translation = XMMatrixTranslation(static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2]));
    return scale * rotation * translation;
}

void ExpandBounds(rb::ImportedScene& scene, const XMFLOAT3& position)
{
    scene.boundsMin.x = std::min(scene.boundsMin.x, position.x);
    scene.boundsMin.y = std::min(scene.boundsMin.y, position.y);
    scene.boundsMin.z = std::min(scene.boundsMin.z, position.z);
    scene.boundsMax.x = std::max(scene.boundsMax.x, position.x);
    scene.boundsMax.y = std::max(scene.boundsMax.y, position.y);
    scene.boundsMax.z = std::max(scene.boundsMax.z, position.z);
}

std::vector<std::uint32_t> PrimitiveIndices(const tinygltf::Model& model, const tinygltf::Primitive& primitive, std::size_t vertexCount)
{
    std::vector<std::uint32_t> source;
    if (primitive.indices >= 0)
    {
        AccessorView indices(model, primitive.indices);
        source.reserve(indices.Count());
        for (std::size_t i = 0; i < indices.Count(); ++i) source.push_back(indices.Index(i));
    }
    else
    {
        source.resize(vertexCount);
        for (std::size_t i = 0; i < vertexCount; ++i) source[i] = static_cast<std::uint32_t>(i);
    }
    std::vector<std::uint32_t> triangles;
    if (primitive.mode == TINYGLTF_MODE_TRIANGLES || primitive.mode == -1)
    {
        triangles = std::move(source);
    }
    else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP)
    {
        for (std::size_t i = 2; i < source.size(); ++i)
        {
            if ((i & 1u) == 0u) triangles.insert(triangles.end(), { source[i - 2], source[i - 1], source[i] });
            else triangles.insert(triangles.end(), { source[i - 1], source[i - 2], source[i] });
        }
    }
    else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN)
    {
        for (std::size_t i = 2; i < source.size(); ++i) triangles.insert(triangles.end(), { source[0], source[i - 1], source[i] });
    }
    return triangles;
}

void ProcessNode(const tinygltf::Model& model, int nodeIndex, const XMMATRIX& parent, rb::ImportedScene& scene)
{
    if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= model.nodes.size()) throw std::runtime_error("glTF node index is out of range.");
    const tinygltf::Node& node = model.nodes[nodeIndex];
    const XMMATRIX world = NodeMatrix(node) * parent;
    const XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
    if (node.mesh >= 0)
    {
        if (static_cast<std::size_t>(node.mesh) >= model.meshes.size()) throw std::runtime_error("glTF mesh index is out of range.");
        for (const tinygltf::Primitive& primitive : model.meshes[node.mesh].primitives)
        {
            const auto positionAttribute = primitive.attributes.find("POSITION");
            if (positionAttribute == primitive.attributes.end()) continue;
            AccessorView positions(model, positionAttribute->second);
            if (positions.Components() < 3 || positions.Count() > UINT32_MAX) throw std::runtime_error("Invalid glTF POSITION accessor.");
            std::optional<AccessorView> normals;
            std::optional<AccessorView> tangents;
            std::optional<AccessorView> texcoord0;
            std::optional<AccessorView> texcoord1;
            if (const auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end()) normals.emplace(model, it->second);
            if (const auto it = primitive.attributes.find("TANGENT"); it != primitive.attributes.end()) tangents.emplace(model, it->second);
            if (const auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end()) texcoord0.emplace(model, it->second);
            if (const auto it = primitive.attributes.find("TEXCOORD_1"); it != primitive.attributes.end()) texcoord1.emplace(model, it->second);
            const std::uint32_t baseVertex = static_cast<std::uint32_t>(scene.vertices.size());
            for (std::size_t i = 0; i < positions.Count(); ++i)
            {
                XMVECTOR position = XMVector3Transform(XMVectorSet(static_cast<float>(positions.At(i, 0)), static_cast<float>(positions.At(i, 1)), static_cast<float>(positions.At(i, 2)), 1.0f), world);
                XMVECTOR normal = normals && normals->Count() == positions.Count()
                    ? XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(static_cast<float>(normals->At(i, 0)), static_cast<float>(normals->At(i, 1)), static_cast<float>(normals->At(i, 2)), 0.0f), normalMatrix))
                    : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                XMVECTOR tangent = tangents && tangents->Count() == positions.Count()
                    ? XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(static_cast<float>(tangents->At(i, 0)), static_cast<float>(tangents->At(i, 1)), static_cast<float>(tangents->At(i, 2)), 0.0f), world))
                    : XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
                rb::SceneVertex vertex{};
                XMFLOAT3 positionValue; XMStoreFloat3(&positionValue, position); positionValue.z = -positionValue.z; vertex.position = positionValue;
                XMFLOAT3 normalValue; XMStoreFloat3(&normalValue, normal); normalValue.z = -normalValue.z; vertex.normal = normalValue;
                XMFLOAT3 tangentValue; XMStoreFloat3(&tangentValue, tangent); tangentValue.z = -tangentValue.z;
                const float handedness = tangents && tangents->Components() >= 4 ? -static_cast<float>(tangents->At(i, 3)) : 1.0f;
                vertex.tangent = XMFLOAT4(tangentValue.x, tangentValue.y, tangentValue.z, handedness);
                if (texcoord0 && texcoord0->Count() == positions.Count() && texcoord0->Components() >= 2) vertex.texcoord = XMFLOAT2(static_cast<float>(texcoord0->At(i, 0)), static_cast<float>(texcoord0->At(i, 1)));
                if (texcoord1 && texcoord1->Count() == positions.Count() && texcoord1->Components() >= 2) vertex.texcoord1 = XMFLOAT2(static_cast<float>(texcoord1->At(i, 0)), static_cast<float>(texcoord1->At(i, 1)));
                scene.vertices.push_back(vertex);
                ExpandBounds(scene, vertex.position);
            }
            std::vector<std::uint32_t> indices = PrimitiveIndices(model, primitive, positions.Count());
            const std::uint32_t startIndex = static_cast<std::uint32_t>(scene.indices.size());
            for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                if (indices[i] >= positions.Count() || indices[i + 1] >= positions.Count() || indices[i + 2] >= positions.Count()) throw std::runtime_error("glTF primitive index exceeds POSITION count.");
                scene.indices.insert(scene.indices.end(), { baseVertex + indices[i], baseVertex + indices[i + 2], baseVertex + indices[i + 1] });
            }
            if (scene.indices.size() > startIndex)
            {
                rb::SceneDraw draw;
                draw.startIndex = startIndex;
                draw.indexCount = static_cast<std::uint32_t>(scene.indices.size()) - startIndex;
                draw.materialIndex = primitive.material >= 0 ? static_cast<std::uint32_t>(primitive.material) : 0u;
                scene.draws.push_back(draw);
            }
        }
    }
    for (int child : node.children) ProcessNode(model, child, world, scene);
}

rb::SceneMaterial ParseMaterial(
    const tinygltf::Model& model,
    const tinygltf::Material& source,
    std::size_t materialIndex,
    const std::filesystem::path& sceneDirectory)
{
    rb::SceneMaterial material{};
    material.sourceMaterialId = "gltf:material/" + std::to_string(materialIndex);
    material.assignment.materialName = source.name.empty() ? "Material " + std::to_string(materialIndex) : source.name;
    material.assignment.shaderSetName = "LookDev glTF PBR";
    const auto& pbr = source.pbrMetallicRoughness;
    material.assignment.baseColorFactor = {
        static_cast<float>(pbr.baseColorFactor[0]), static_cast<float>(pbr.baseColorFactor[1]),
        static_cast<float>(pbr.baseColorFactor[2]), static_cast<float>(pbr.baseColorFactor[3]) };
    material.baseColorFactor = XMFLOAT4(material.assignment.baseColorFactor.data());
    material.assignment.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
    material.assignment.metallicFactor = static_cast<float>(pbr.metallicFactor);
    material.assignment.normalStrength = static_cast<float>(source.normalTexture.scale);
    material.assignment.occlusionStrength = static_cast<float>(source.occlusionTexture.strength);
    material.assignment.alphaCutoff = static_cast<float>(source.alphaCutoff);
    material.assignment.alphaMode = source.alphaMode == "MASK" ? rb::AlphaMode::Mask : source.alphaMode == "BLEND" ? rb::AlphaMode::Blend : rb::AlphaMode::Opaque;
    material.assignment.emissiveFactor = {
        static_cast<float>(source.emissiveFactor[0]), static_cast<float>(source.emissiveFactor[1]),
        static_cast<float>(source.emissiveFactor[2]), 1.0f };
    material.emissiveFactor = XMFLOAT4(material.assignment.emissiveFactor.data());
    material.twoSidedEmission = source.doubleSided;

    std::uint32_t featureMask = 0;
    AssignTexture(material, rb::TextureSlot::BaseColor, ParseTextureInfo(pbr.baseColorTexture), model, sceneDirectory, featureMask);
    AssignTexture(material, rb::TextureSlot::Normal, ParseTextureInfo(source.normalTexture), model, sceneDirectory, featureMask);
    const ParsedTextureInfo metallicRoughness = ParseTextureInfo(pbr.metallicRoughnessTexture);
    AssignTexture(material, rb::TextureSlot::Roughness, metallicRoughness, model, sceneDirectory, featureMask);
    AssignTexture(material, rb::TextureSlot::Metallic, metallicRoughness, model, sceneDirectory, featureMask);
    AssignTexture(material, rb::TextureSlot::Occlusion, ParseTextureInfo(source.occlusionTexture), model, sceneDirectory, featureMask);
    AssignTexture(material, rb::TextureSlot::Emissive, ParseTextureInfo(source.emissiveTexture), model, sceneDirectory, featureMask);

    const auto parseExtension = [&](const char* name) -> const tinygltf::Value*
    {
        const auto found = source.extensions.find(name);
        return found == source.extensions.end() ? nullptr : &found->second;
    };
    if (const tinygltf::Value* extension = parseExtension("KHR_materials_specular"))
    {
        featureMask |= rb::GltfMaterialFeatureSpecular;
        material.gltfExtensions.specularFactor = static_cast<float>(Number(*extension, "specularFactor", 1.0));
        material.gltfExtensions.specularColorFactor = Vec3(*extension, "specularColorFactor", { 1.0f, 1.0f, 1.0f });
        if (const tinygltf::Value* texture = Member(*extension, "specularTexture")) AssignTexture(material, rb::TextureSlot::SpecularFactor, ParseTextureValue(*texture), model, sceneDirectory, featureMask);
        if (const tinygltf::Value* texture = Member(*extension, "specularColorTexture")) AssignTexture(material, rb::TextureSlot::SpecularColor, ParseTextureValue(*texture), model, sceneDirectory, featureMask);
    }
    if (const tinygltf::Value* extension = parseExtension("KHR_materials_ior"))
    {
        featureMask |= rb::GltfMaterialFeatureIor;
        material.gltfExtensions.ior = static_cast<float>(Number(*extension, "ior", 1.5));
    }
    if (const tinygltf::Value* extension = parseExtension("KHR_materials_transmission"))
    {
        featureMask |= rb::GltfMaterialFeatureTransmission;
        material.gltfExtensions.transmissionFactor = static_cast<float>(Number(*extension, "transmissionFactor", 0.0));
        if (const tinygltf::Value* texture = Member(*extension, "transmissionTexture")) AssignTexture(material, rb::TextureSlot::Transmission, ParseTextureValue(*texture), model, sceneDirectory, featureMask);
    }
    if (const tinygltf::Value* extension = parseExtension("KHR_materials_volume"))
    {
        featureMask |= rb::GltfMaterialFeatureVolume | rb::GltfMaterialFeatureTransmission;
        material.gltfExtensions.thicknessFactor = static_cast<float>(Number(*extension, "thicknessFactor", 0.0));
        material.gltfExtensions.attenuationColor = Vec3(*extension, "attenuationColor", { 1.0f, 1.0f, 1.0f });
        material.gltfExtensions.attenuationDistance = static_cast<float>(Number(*extension, "attenuationDistance", std::numeric_limits<float>::max()));
        if (const tinygltf::Value* texture = Member(*extension, "thicknessTexture")) AssignTexture(material, rb::TextureSlot::Thickness, ParseTextureValue(*texture), model, sceneDirectory, featureMask);
    }
    if (const tinygltf::Value* extension = parseExtension("KHR_materials_clearcoat"))
    {
        featureMask |= rb::GltfMaterialFeatureClearcoat;
        material.gltfExtensions.clearcoatFactor = static_cast<float>(Number(*extension, "clearcoatFactor", 0.0));
        material.gltfExtensions.clearcoatRoughnessFactor = static_cast<float>(Number(*extension, "clearcoatRoughnessFactor", 0.0));
        if (const tinygltf::Value* texture = Member(*extension, "clearcoatTexture")) AssignTexture(material, rb::TextureSlot::Clearcoat, ParseTextureValue(*texture), model, sceneDirectory, featureMask);
        if (const tinygltf::Value* texture = Member(*extension, "clearcoatRoughnessTexture")) AssignTexture(material, rb::TextureSlot::ClearcoatRoughness, ParseTextureValue(*texture), model, sceneDirectory, featureMask);
        if (const tinygltf::Value* texture = Member(*extension, "clearcoatNormalTexture"))
        {
            material.gltfExtensions.clearcoatNormalScale = static_cast<float>(Number(*texture, "scale", 1.0));
            AssignTexture(material, rb::TextureSlot::ClearcoatNormal, ParseTextureValue(*texture), model, sceneDirectory, featureMask);
        }
    }

    material.gltfExtensions.featureMask = featureMask;
    material.assignment.gltfExtensions = material.gltfExtensions;
    material.transmissionFactor = material.gltfExtensions.transmissionFactor;
    material.indexOfRefraction = material.gltfExtensions.ior;
    material.thinDielectric = material.gltfExtensions.thicknessFactor <= 0.0f;
    material.assignment.textureBindings = material.textureBindings;
    material.baseColorTexturePath = material.textureBindings[static_cast<std::size_t>(rb::TextureSlot::BaseColor)].path;
    material.normalTexturePath = material.textureBindings[static_cast<std::size_t>(rb::TextureSlot::Normal)].path;
    material.roughnessTexturePath = material.textureBindings[static_cast<std::size_t>(rb::TextureSlot::Roughness)].path;
    material.metallicTexturePath = material.textureBindings[static_cast<std::size_t>(rb::TextureSlot::Metallic)].path;
    material.occlusionTexturePath = material.textureBindings[static_cast<std::size_t>(rb::TextureSlot::Occlusion)].path;
    material.emissiveTexturePath = material.textureBindings[static_cast<std::size_t>(rb::TextureSlot::Emissive)].path;
    material.hasBaseColorTexture = !material.baseColorTexturePath.empty();
    material.hasNormalTexture = !material.normalTexturePath.empty();
    material.hasRoughnessTexture = !material.roughnessTexturePath.empty();
    material.hasMetallicTexture = !material.metallicTexturePath.empty();
    material.hasOcclusionTexture = !material.occlusionTexturePath.empty();
    material.hasEmissiveTexture = !material.emissiveTexturePath.empty();
    return material;
}
}

namespace rb
{
SceneImportResult ImportGltfScene(const std::wstring& path)
{
    SceneImportResult result;
    try
    {
        const std::filesystem::path scenePath(path);
        tinygltf::TinyGLTF loader;
        loader.SetImageLoader(PreserveEncodedImage, nullptr);
        tinygltf::Model model;
        std::string errors;
        std::string warnings;
        const bool binary = SceneExtensionEquals(scenePath.extension().wstring(), L".glb");
        const bool loaded = binary
            ? loader.LoadBinaryFromFile(&model, &errors, &warnings, WideToUtf8(scenePath.wstring()))
            : loader.LoadASCIIFromFile(&model, &errors, &warnings, WideToUtf8(scenePath.wstring()));
        if (!loaded)
        {
            result.diagnostics = errors.empty() ? "tinygltf failed to read the scene." : errors;
            return result;
        }

        static const std::set<std::string> SupportedExtensions = {
            "KHR_texture_transform", "KHR_texture_basisu", "KHR_materials_specular",
            "KHR_materials_ior", "KHR_materials_transmission", "KHR_materials_volume",
            "KHR_materials_clearcoat" };
        result.scene.extensionsUsed = model.extensionsUsed;
        result.scene.extensionsRequired = model.extensionsRequired;
        for (const std::string& extension : model.extensionsUsed)
        {
            if (!SupportedExtensions.contains(extension)) result.scene.unsupportedExtensions.push_back(extension);
        }
        for (const std::string& extension : model.extensionsRequired)
        {
            if (!SupportedExtensions.contains(extension))
            {
                result.diagnostics = "Unsupported required glTF extension: " + extension;
                return result;
            }
        }

        result.scene.path = path;
        result.scene.boundsMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        result.scene.boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        const std::filesystem::path sceneDirectory = scenePath.parent_path();
        if (model.materials.empty())
        {
            tinygltf::Material fallback;
            result.scene.materials.push_back(ParseMaterial(model, fallback, 0, sceneDirectory));
        }
        else
        {
            result.scene.materials.reserve(model.materials.size());
            for (std::size_t i = 0; i < model.materials.size(); ++i)
            {
                result.scene.materials.push_back(ParseMaterial(model, model.materials[i], i, sceneDirectory));
                result.scene.materialFeatureMask |= result.scene.materials.back().gltfExtensions.featureMask;
            }
        }

        int sceneIndex = model.defaultScene >= 0 ? model.defaultScene : (model.scenes.empty() ? -1 : 0);
        if (sceneIndex < 0 || static_cast<std::size_t>(sceneIndex) >= model.scenes.size())
        {
            result.diagnostics = "glTF does not define a renderable scene.";
            return result;
        }
        for (int rootNode : model.scenes[sceneIndex].nodes) ProcessNode(model, rootNode, XMMatrixIdentity(), result.scene);
        if (result.scene.vertices.empty() || result.scene.indices.empty() || result.scene.draws.empty())
        {
            result.diagnostics = "The glTF scene did not contain renderable triangle meshes.";
            result.scene = {};
            return result;
        }
        SceneMesh canonical;
        canonical.name = WideToUtf8(scenePath.filename().wstring());
        canonical.vertices = result.scene.vertices;
        canonical.indices = result.scene.indices;
        canonical.draws = result.scene.draws;
        canonical.boundsMin = result.scene.boundsMin;
        canonical.boundsMax = result.scene.boundsMax;
        result.scene.meshes.push_back(std::move(canonical));
        SceneInstance instance;
        instance.meshIndex = 0;
        XMStoreFloat4x4(&instance.transform, XMMatrixIdentity());
        XMStoreFloat4x4(&instance.normalTransform, XMMatrixIdentity());
        result.scene.instances.push_back(instance);
        std::ostringstream diagnostics;
        diagnostics << "Loaded " << scenePath.filename().string() << " through tinygltf with "
            << result.scene.vertices.size() << " vertices, " << result.scene.draws.size()
            << " draws, and " << result.scene.materials.size() << " materials.";
        if (!warnings.empty()) diagnostics << "\n" << warnings;
        if (!result.scene.unsupportedExtensions.empty()) diagnostics << "\nOptional unsupported glTF extensions use core fallbacks.";
        result.diagnostics = diagnostics.str();
        result.succeeded = true;
    }
    catch (const std::exception& exception)
    {
        result.diagnostics = exception.what();
        result.scene = {};
    }
    return result;
}
}

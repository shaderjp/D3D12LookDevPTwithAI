#include "PbrtSceneImporter.h"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>

using namespace DirectX;

namespace
{
constexpr std::size_t MaxIncludeDepth = 64;
constexpr std::size_t MaxDiagnosticExamples = 8;

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        std::string fallback;
        fallback.reserve(text.size());
        for (wchar_t ch : text) fallback.push_back(static_cast<char>(ch & 0x7f));
        return fallback;
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        std::wstring fallback;
        fallback.reserve(text.size());
        for (unsigned char ch : text) fallback.push_back(static_cast<wchar_t>(ch));
        return fallback;
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::filesystem::path CanonicalPath(const std::filesystem::path& value)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(value, ec);
    if (ec) absolute = value;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
    return ec ? absolute.lexically_normal() : canonical;
}

struct SourceLocation
{
    std::filesystem::path file;
    std::uint32_t line = 1;
    std::uint32_t column = 1;
};

class PbrtError final : public std::runtime_error
{
public:
    PbrtError(const SourceLocation& location, const std::string& message)
        : std::runtime_error(Format(location, message)) {}

private:
    static std::string Format(const SourceLocation& location, const std::string& message)
    {
        std::ostringstream stream;
        stream << WideToUtf8(location.file.wstring()) << ':' << location.line << ':' << location.column << ": " << message;
        return stream.str();
    }
};

enum class TokenKind
{
    Identifier,
    String,
    Number,
    LeftBracket,
    RightBracket,
    End,
};

struct Token
{
    TokenKind kind = TokenKind::End;
    std::string text;
    SourceLocation location;
};

class Lexer
{
public:
    Lexer(std::filesystem::path file, std::string source)
        : m_file(std::move(file)), m_source(std::move(source)) {}

    std::vector<Token> Tokenize()
    {
        std::vector<Token> tokens;
        while (true)
        {
            SkipWhitespaceAndComments();
            SourceLocation location{ m_file, m_line, m_column };
            if (m_offset >= m_source.size())
            {
                tokens.push_back({ TokenKind::End, {}, location });
                return tokens;
            }

            const char ch = m_source[m_offset];
            if (ch == '[' || ch == ']')
            {
                Advance();
                tokens.push_back({ ch == '[' ? TokenKind::LeftBracket : TokenKind::RightBracket, std::string(1, ch), location });
            }
            else if (ch == '"')
            {
                tokens.push_back(ReadString(location));
            }
            else
            {
                tokens.push_back(ReadBare(location));
            }
        }
    }

private:
    void Advance()
    {
        if (m_source[m_offset++] == '\n')
        {
            ++m_line;
            m_column = 1;
        }
        else
        {
            ++m_column;
        }
    }

    void SkipWhitespaceAndComments()
    {
        while (m_offset < m_source.size())
        {
            if (std::isspace(static_cast<unsigned char>(m_source[m_offset])))
            {
                Advance();
                continue;
            }
            if (m_source[m_offset] == '#')
            {
                while (m_offset < m_source.size() && m_source[m_offset] != '\n') Advance();
                continue;
            }
            break;
        }
    }

    Token ReadString(const SourceLocation& location)
    {
        Advance();
        std::string value;
        while (m_offset < m_source.size())
        {
            const char ch = m_source[m_offset];
            if (ch == '"')
            {
                Advance();
                return { TokenKind::String, value, location };
            }
            if (ch == '\\')
            {
                Advance();
                if (m_offset >= m_source.size()) break;
                const char escaped = m_source[m_offset];
                switch (escaped)
                {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(escaped); break;
                }
                Advance();
                continue;
            }
            value.push_back(ch);
            Advance();
        }
        throw PbrtError(location, "Unterminated quoted string.");
    }

    Token ReadBare(const SourceLocation& location)
    {
        const std::size_t begin = m_offset;
        while (m_offset < m_source.size())
        {
            const char ch = m_source[m_offset];
            if (std::isspace(static_cast<unsigned char>(ch)) || ch == '[' || ch == ']' || ch == '"' || ch == '#') break;
            Advance();
        }
        if (begin == m_offset) throw PbrtError(location, "Unexpected character.");
        std::string text = m_source.substr(begin, m_offset - begin);
        char* end = nullptr;
        std::strtod(text.c_str(), &end);
        const TokenKind kind = end && *end == '\0' && end != text.c_str() ? TokenKind::Number : TokenKind::Identifier;
        return { kind, std::move(text), location };
    }

    std::filesystem::path m_file;
    std::string m_source;
    std::size_t m_offset = 0;
    std::uint32_t m_line = 1;
    std::uint32_t m_column = 1;
};

struct Parameter
{
    std::string type;
    std::vector<double> numbers;
    std::vector<std::string> strings;
    SourceLocation location;
};

using ParameterMap = std::unordered_map<std::string, Parameter>;

std::pair<std::string, std::string> SplitParameterDeclaration(const Token& token)
{
    const std::size_t separator = token.text.find_first_of(" \t");
    if (separator == std::string::npos) throw PbrtError(token.location, "Parameter declaration must contain a type and name.");
    std::size_t name = token.text.find_first_not_of(" \t", separator);
    if (name == std::string::npos) throw PbrtError(token.location, "Parameter declaration is missing its name.");
    return { Lower(token.text.substr(0, separator)), Lower(token.text.substr(name)) };
}

XMFLOAT4X4 StoreMatrix(FXMMATRIX matrix)
{
    XMFLOAT4X4 value;
    XMStoreFloat4x4(&value, matrix);
    return value;
}

XMMATRIX LoadMatrix(const XMFLOAT4X4& matrix)
{
    return XMLoadFloat4x4(&matrix);
}

XMFLOAT3 TransformPoint(const XMFLOAT3& point, FXMMATRIX matrix)
{
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3TransformCoord(XMLoadFloat3(&point), matrix));
    return result;
}

XMFLOAT3 TransformDirection(const XMFLOAT3& direction, FXMMATRIX matrix)
{
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&direction), matrix)));
    return result;
}

void InitializeMeshBounds(rb::SceneMesh& mesh)
{
    mesh.boundsMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    mesh.boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
}

void ExpandBounds(XMFLOAT3& minimum, XMFLOAT3& maximum, const XMFLOAT3& point)
{
    minimum.x = (std::min)(minimum.x, point.x);
    minimum.y = (std::min)(minimum.y, point.y);
    minimum.z = (std::min)(minimum.z, point.z);
    maximum.x = (std::max)(maximum.x, point.x);
    maximum.y = (std::max)(maximum.y, point.y);
    maximum.z = (std::max)(maximum.z, point.z);
}

void GenerateNormalsAndTangents(rb::SceneMesh& mesh, bool normalsPresent)
{
    std::vector<XMVECTOR> normalSums(mesh.vertices.size(), XMVectorZero());
    std::vector<XMVECTOR> tangentSums(mesh.vertices.size(), XMVectorZero());
    std::vector<XMVECTOR> bitangentSums(mesh.vertices.size(), XMVectorZero());
    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
    {
        const std::uint32_t i0 = mesh.indices[index];
        const std::uint32_t i1 = mesh.indices[index + 1];
        const std::uint32_t i2 = mesh.indices[index + 2];
        if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) continue;
        const XMVECTOR p0 = XMLoadFloat3(&mesh.vertices[i0].position);
        const XMVECTOR p1 = XMLoadFloat3(&mesh.vertices[i1].position);
        const XMVECTOR p2 = XMLoadFloat3(&mesh.vertices[i2].position);
        const XMVECTOR edge0 = p1 - p0;
        const XMVECTOR edge1 = p2 - p0;
        const XMVECTOR faceNormal = XMVector3Cross(edge0, edge1);
        if (!normalsPresent)
        {
            normalSums[i0] += faceNormal;
            normalSums[i1] += faceNormal;
            normalSums[i2] += faceNormal;
        }

        const XMFLOAT2 uv0 = mesh.vertices[i0].texcoord;
        const XMFLOAT2 uv1 = mesh.vertices[i1].texcoord;
        const XMFLOAT2 uv2 = mesh.vertices[i2].texcoord;
        const float du0 = uv1.x - uv0.x;
        const float dv0 = uv1.y - uv0.y;
        const float du1 = uv2.x - uv0.x;
        const float dv1 = uv2.y - uv0.y;
        const float determinant = du0 * dv1 - dv0 * du1;
        XMVECTOR tangent = edge0;
        XMVECTOR bitangent = XMVector3Cross(faceNormal, tangent);
        if (std::abs(determinant) > 1e-12f)
        {
            const float reciprocal = 1.0f / determinant;
            tangent = (edge0 * dv1 - edge1 * dv0) * reciprocal;
            bitangent = (edge1 * du0 - edge0 * du1) * reciprocal;
        }
        tangentSums[i0] += tangent; tangentSums[i1] += tangent; tangentSums[i2] += tangent;
        bitangentSums[i0] += bitangent; bitangentSums[i1] += bitangent; bitangentSums[i2] += bitangent;
    }

    for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
    {
        XMVECTOR normal = normalsPresent ? XMLoadFloat3(&mesh.vertices[index].normal) : normalSums[index];
        if (XMVectorGetX(XMVector3LengthSq(normal)) < 1e-16f) normal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        normal = XMVector3Normalize(normal);
        XMVECTOR tangent = tangentSums[index] - normal * XMVector3Dot(normal, tangentSums[index]);
        if (XMVectorGetX(XMVector3LengthSq(tangent)) < 1e-16f)
        {
            tangent = std::abs(XMVectorGetY(normal)) < 0.999f
                ? XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), normal)
                : XMVector3Cross(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), normal);
        }
        tangent = XMVector3Normalize(tangent);
        const float sign = XMVectorGetX(XMVector3Dot(XMVector3Cross(normal, tangent), bitangentSums[index])) < 0.0f ? -1.0f : 1.0f;
        XMStoreFloat3(&mesh.vertices[index].normal, normal);
        XMStoreFloat4(&mesh.vertices[index].tangent, XMVectorSetW(tangent, sign));
    }
}

struct TextureDefinition
{
    std::string valueType;
    std::string implementation;
    ParameterMap parameters;
    std::filesystem::path sourceDirectory;
};

struct AreaLightState
{
    bool enabled = false;
    XMFLOAT3 radiance = XMFLOAT3(0.0f, 0.0f, 0.0f);
    bool twoSided = false;
};

struct GraphicsState
{
    XMFLOAT4X4 transform = StoreMatrix(XMMatrixIdentity());
    std::uint32_t materialIndex = 0;
    bool reverseOrientation = false;
    AreaLightState areaLight;
};

struct MeshElement
{
    std::uint32_t meshIndex = 0;
    XMFLOAT4X4 transform = StoreMatrix(XMMatrixIdentity());
    SourceLocation location;
};

struct ObjectElement
{
    std::string name;
    XMFLOAT4X4 transform = StoreMatrix(XMMatrixIdentity());
    SourceLocation location;
};

using ObjectDefinitionElement = std::variant<MeshElement, ObjectElement>;

struct WarningBucket
{
    std::uint64_t count = 0;
    std::vector<std::string> examples;
};

class PbrtImporter
{
public:
    PbrtImporter(
        std::filesystem::path rootPath,
        const std::atomic_bool* cancelRequested,
        rb::SceneImportProgressCallback progress)
        : m_rootPath(CanonicalPath(rootPath)),
          m_cancelRequested(cancelRequested),
          m_progress(std::move(progress))
    {
        rb::SceneMaterial material;
        material.assignment = { "PBRT Default", "LookDev PBR" };
        material.assignment.roughnessFactor = 1.0f;
        material.baseColorFactor = XMFLOAT4(0.5f, 0.5f, 0.5f, 1.0f);
        material.assignment.baseColorFactor = { 0.5f, 0.5f, 0.5f, 1.0f };
        m_scene.materials.push_back(material);
        m_namedMaterials["__default"] = 0;
        m_coordinateSystems["world"] = StoreMatrix(XMMatrixIdentity());
    }

    rb::SceneImportResult Run()
    {
        rb::SceneImportResult result;
        try
        {
            ReportProgress(rb::SceneImportProgress::Stage::Parsing, 0, 0, m_rootPath.wstring());
            ParseFile(m_rootPath, 0);
            CheckCancelled(SourceLocation{ m_rootPath, 1, 1 });
            ResolveRootObjects();
            if (m_scene.meshes.empty() || m_scene.instances.empty())
            {
                throw PbrtError(SourceLocation{ m_rootPath, 1, 1 }, "The PBRT scene did not contain supported renderable triangle meshes.");
            }
            ComputeWorldBounds();
            m_scene.path = m_rootPath.wstring();
            result.scene = std::move(m_scene);
            result.succeeded = true;
            result.diagnostics = BuildDiagnostics(result.scene);
        }
        catch (const std::exception& exception)
        {
            result.diagnostics = exception.what();
        }
        return result;
    }

private:
    void ReportProgress(rb::SceneImportProgress::Stage stage, std::uint64_t completed, std::uint64_t total, const std::wstring& current)
    {
        if (m_progress) m_progress({ stage, completed, total, current });
    }

    void CheckCancelled(const SourceLocation& location) const
    {
        if (m_cancelRequested && m_cancelRequested->load(std::memory_order_relaxed))
        {
            throw PbrtError(location, "Scene import was cancelled.");
        }
    }

    void Warn(const std::string& category, const std::string& example = {})
    {
        WarningBucket& bucket = m_warnings[category];
        ++bucket.count;
        if (!example.empty() && bucket.examples.size() < MaxDiagnosticExamples &&
            std::find(bucket.examples.begin(), bucket.examples.end(), example) == bucket.examples.end())
        {
            bucket.examples.push_back(example);
        }
    }

    static std::string ReadFileUtf8(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw PbrtError(SourceLocation{ path, 1, 1 }, "Unable to open file.");
        std::ostringstream stream;
        stream << file.rdbuf();
        std::string result = stream.str();
        if (result.size() >= 3 && static_cast<unsigned char>(result[0]) == 0xef &&
            static_cast<unsigned char>(result[1]) == 0xbb && static_cast<unsigned char>(result[2]) == 0xbf)
        {
            result.erase(0, 3);
        }
        return result;
    }

    static const Token& Require(const std::vector<Token>& tokens, std::size_t& cursor, TokenKind kind, const char* description)
    {
        if (cursor >= tokens.size() || tokens[cursor].kind != kind)
        {
            const SourceLocation location = cursor < tokens.size() ? tokens[cursor].location : tokens.back().location;
            throw PbrtError(location, std::string("Expected ") + description + '.');
        }
        return tokens[cursor++];
    }

    static double RequireNumber(const std::vector<Token>& tokens, std::size_t& cursor)
    {
        const Token& token = Require(tokens, cursor, TokenKind::Number, "a number");
        const double value = std::strtod(token.text.c_str(), nullptr);
        if (!std::isfinite(value)) throw PbrtError(token.location, "Non-finite numeric value is not supported.");
        return value;
    }

    static std::string RequireName(const std::vector<Token>& tokens, std::size_t& cursor, const char* description)
    {
        if (cursor >= tokens.size() || (tokens[cursor].kind != TokenKind::String && tokens[cursor].kind != TokenKind::Identifier))
        {
            const SourceLocation location = cursor < tokens.size() ? tokens[cursor].location : tokens.back().location;
            throw PbrtError(location, std::string("Expected ") + description + '.');
        }
        return tokens[cursor++].text;
    }

    static ParameterMap ReadParameters(const std::vector<Token>& tokens, std::size_t& cursor)
    {
        ParameterMap parameters;
        while (cursor < tokens.size() && tokens[cursor].kind == TokenKind::String)
        {
            const Token declaration = tokens[cursor++];
            const auto [type, name] = SplitParameterDeclaration(declaration);
            Parameter parameter;
            parameter.type = type;
            parameter.location = declaration.location;
            const bool array = cursor < tokens.size() && tokens[cursor].kind == TokenKind::LeftBracket;
            if (array) ++cursor;
            bool readAny = false;
            while (cursor < tokens.size())
            {
                if (array && tokens[cursor].kind == TokenKind::RightBracket)
                {
                    ++cursor;
                    break;
                }
                if (!array && readAny) break;
                const Token& value = tokens[cursor];
                if (value.kind == TokenKind::Number)
                {
                    parameter.numbers.push_back(RequireNumber(tokens, cursor));
                }
                else if (value.kind == TokenKind::String || value.kind == TokenKind::Identifier)
                {
                    parameter.strings.push_back(value.text);
                    ++cursor;
                }
                else
                {
                    throw PbrtError(value.location, "Invalid parameter value.");
                }
                readAny = true;
            }
            if (!readAny) throw PbrtError(declaration.location, "Parameter has no value.");
            parameters[name] = std::move(parameter);
        }
        return parameters;
    }

    static const Parameter* FindParameter(const ParameterMap& parameters, const std::string& name)
    {
        const auto found = parameters.find(Lower(name));
        return found == parameters.end() ? nullptr : &found->second;
    }

    static double NumberOr(const ParameterMap& parameters, const std::string& name, double fallback)
    {
        const Parameter* parameter = FindParameter(parameters, name);
        return parameter && !parameter->numbers.empty() ? parameter->numbers.front() : fallback;
    }

    static bool BoolOr(const ParameterMap& parameters, const std::string& name, bool fallback)
    {
        const Parameter* parameter = FindParameter(parameters, name);
        if (!parameter) return fallback;
        if (!parameter->numbers.empty()) return parameter->numbers.front() != 0.0;
        if (!parameter->strings.empty())
        {
            const std::string value = Lower(parameter->strings.front());
            if (value == "true") return true;
            if (value == "false") return false;
        }
        return fallback;
    }

    static std::string StringOr(const ParameterMap& parameters, const std::string& name, std::string fallback = {})
    {
        const Parameter* parameter = FindParameter(parameters, name);
        return parameter && !parameter->strings.empty() ? parameter->strings.front() : std::move(fallback);
    }

    static XMFLOAT3 BlackbodyToRgb(float temperature)
    {
        temperature = std::clamp(temperature, 1000.0f, 40000.0f) / 100.0f;
        float red = temperature <= 66.0f ? 255.0f : 329.698727446f * std::pow(temperature - 60.0f, -0.1332047592f);
        float green = temperature <= 66.0f
            ? 99.4708025861f * std::log(temperature) - 161.1195681661f
            : 288.1221695283f * std::pow(temperature - 60.0f, -0.0755148492f);
        float blue = temperature >= 66.0f ? 255.0f : temperature <= 19.0f ? 0.0f : 138.5177312231f * std::log(temperature - 10.0f) - 305.044792731f;
        auto linear = [](float value)
        {
            value = std::clamp(value / 255.0f, 0.0f, 1.0f);
            return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
        };
        return XMFLOAT3(linear(red), linear(green), linear(blue));
    }

    XMFLOAT3 ColorOr(const ParameterMap& parameters, const std::string& name, const XMFLOAT3& fallback)
    {
        const Parameter* parameter = FindParameter(parameters, name);
        if (!parameter) return fallback;
        if (parameter->type == "blackbody" && !parameter->numbers.empty())
        {
            XMFLOAT3 color = BlackbodyToRgb(static_cast<float>(parameter->numbers[0]));
            const float scale = parameter->numbers.size() > 1 ? static_cast<float>(parameter->numbers[1]) : 1.0f;
            color.x *= scale; color.y *= scale; color.z *= scale;
            return color;
        }
        if (parameter->type == "spectrum" && !parameter->numbers.empty())
        {
            Warn("Unsupported sampled spectrum", name);
            return fallback;
        }
        if (parameter->numbers.size() >= 3)
        {
            return XMFLOAT3(
                static_cast<float>(parameter->numbers[0]),
                static_cast<float>(parameter->numbers[1]),
                static_cast<float>(parameter->numbers[2]));
        }
        if (parameter->numbers.size() == 1)
        {
            const float value = static_cast<float>(parameter->numbers[0]);
            return XMFLOAT3(value, value, value);
        }
        if (!parameter->strings.empty())
        {
            static const std::unordered_map<std::string, XMFLOAT3> named =
            {
                { "metal-ag-eta", XMFLOAT3(0.97f, 0.96f, 0.91f) },
                { "metal-al-eta", XMFLOAT3(0.91f, 0.92f, 0.92f) },
                { "metal-au-eta", XMFLOAT3(1.00f, 0.71f, 0.29f) },
                { "metal-cu-eta", XMFLOAT3(0.95f, 0.64f, 0.54f) },
            };
            const auto found = named.find(Lower(parameter->strings.front()));
            if (found != named.end()) return found->second;
            Warn("Unsupported named or file spectrum", parameter->strings.front());
        }
        return fallback;
    }

    static std::optional<XMFLOAT3> NamedConductorF0(const ParameterMap& parameters)
    {
        const Parameter* eta = FindParameter(parameters, "eta");
        if (!eta || eta->strings.empty()) return std::nullopt;
        const std::string name = Lower(eta->strings.front());
        if (name.find("metal-ag-") == 0) return XMFLOAT3(0.95f, 0.93f, 0.88f);
        if (name.find("metal-al-") == 0) return XMFLOAT3(0.91f, 0.92f, 0.92f);
        if (name.find("metal-au-") == 0) return XMFLOAT3(1.00f, 0.77f, 0.34f);
        if (name.find("metal-cu-") == 0) return XMFLOAT3(0.95f, 0.64f, 0.54f);
        return std::nullopt;
    }

    std::filesystem::path ResolveAssetPath(const std::filesystem::path& directory, const std::string& authored) const
    {
        std::filesystem::path path = Utf8ToWide(authored);
        return CanonicalPath(path.is_absolute() ? path : directory / path);
    }

    std::wstring ResolveTextureImagePath(const std::string& name, std::unordered_set<std::string>& stack)
    {
        const auto found = m_textures.find(name);
        if (found == m_textures.end())
        {
            Warn("Missing named texture", name);
            return {};
        }
        if (!stack.insert(name).second)
        {
            Warn("Cyclic texture graph", name);
            return {};
        }
        const TextureDefinition& texture = found->second;
        std::wstring result;
        if (texture.implementation == "imagemap")
        {
            const std::string authored = StringOr(texture.parameters, "filename");
            if (!authored.empty())
            {
                const std::filesystem::path resolved = ResolveAssetPath(texture.sourceDirectory, authored);
                if (std::filesystem::exists(resolved)) result = resolved.wstring();
                else Warn("Missing optional texture", WideToUtf8(resolved.wstring()));
            }
        }
        else if (texture.implementation == "scale")
        {
            const Parameter* input = FindParameter(texture.parameters, "tex");
            if (!input) input = FindParameter(texture.parameters, "tex1");
            if (input && input->type == "texture" && !input->strings.empty())
                result = ResolveTextureImagePath(input->strings.front(), stack);
        }
        else if (texture.implementation == "mix")
        {
            const float amount = static_cast<float>(NumberOr(texture.parameters, "amount", 0.5));
            const Parameter* first = FindParameter(texture.parameters, "tex1");
            const Parameter* second = FindParameter(texture.parameters, "tex2");
            auto resolve = [&](const Parameter* parameter)
            {
                return parameter && parameter->type == "texture" && !parameter->strings.empty()
                    ? ResolveTextureImagePath(parameter->strings.front(), stack)
                    : std::wstring{};
            };
            if (amount <= 0.0f) result = resolve(first);
            else if (amount >= 1.0f) result = resolve(second);
            else
            {
                const std::wstring firstPath = resolve(first);
                const std::wstring secondPath = resolve(second);
                if (!firstPath.empty() && firstPath == secondPath) result = firstPath;
                else Warn("Unsupported texture graph", name + " (non-trivial mix)");
            }
        }
        else if (texture.implementation != "constant")
        {
            Warn("Unsupported texture graph", name + " (" + texture.implementation + ")");
        }
        stack.erase(name);
        return result;
    }

    std::wstring TexturePathFromParameter(
        const ParameterMap& parameters,
        const std::string& parameterName,
        const std::filesystem::path& sourceDirectory)
    {
        const Parameter* parameter = FindParameter(parameters, parameterName);
        if (!parameter || parameter->strings.empty()) return {};
        std::string authored;
        std::filesystem::path directory = sourceDirectory;
        if (parameter->type == "texture")
        {
            std::unordered_set<std::string> stack;
            return ResolveTextureImagePath(parameter->strings.front(), stack);
        }
        else if (parameter->type == "string")
        {
            authored = parameter->strings.front();
        }
        if (authored.empty()) return {};
        const std::filesystem::path resolved = ResolveAssetPath(directory, authored);
        if (!std::filesystem::exists(resolved))
        {
            Warn("Missing optional texture", WideToUtf8(resolved.wstring()));
            return {};
        }
        return resolved.wstring();
    }

    std::optional<XMFLOAT4> ResolveTextureUvTransform(
        const std::string& name,
        std::unordered_set<std::string>& stack)
    {
        const auto found = m_textures.find(name);
        if (found == m_textures.end() || !stack.insert(name).second) return std::nullopt;
        const TextureDefinition& texture = found->second;
        std::optional<XMFLOAT4> result;
        if (texture.implementation == "imagemap")
        {
            result = XMFLOAT4(
                static_cast<float>(NumberOr(texture.parameters, "uscale", 1.0)),
                static_cast<float>(NumberOr(texture.parameters, "vscale", 1.0)),
                static_cast<float>(NumberOr(texture.parameters, "udelta", 0.0)),
                static_cast<float>(NumberOr(texture.parameters, "vdelta", 0.0)));
        }
        else if (texture.implementation == "scale")
        {
            const Parameter* input = FindParameter(texture.parameters, "tex");
            if (!input) input = FindParameter(texture.parameters, "tex1");
            if (input && input->type == "texture" && !input->strings.empty())
                result = ResolveTextureUvTransform(input->strings.front(), stack);
        }
        else if (texture.implementation == "mix")
        {
            const float amount = std::clamp(static_cast<float>(NumberOr(texture.parameters, "amount", 0.5)), 0.0f, 1.0f);
            const Parameter* input = FindParameter(texture.parameters, amount < 0.5f ? "tex1" : "tex2");
            if (input && input->type == "texture" && !input->strings.empty())
                result = ResolveTextureUvTransform(input->strings.front(), stack);
        }
        stack.erase(name);
        return result;
    }

    std::optional<XMFLOAT4> TextureUvTransformFromParameter(
        const ParameterMap& parameters,
        const std::string& parameterName)
    {
        const Parameter* parameter = FindParameter(parameters, parameterName);
        if (!parameter || parameter->type != "texture" || parameter->strings.empty()) return std::nullopt;
        std::unordered_set<std::string> stack;
        return ResolveTextureUvTransform(parameter->strings.front(), stack);
    }

    XMFLOAT3 TextureColorOr(
        const ParameterMap& parameters,
        const std::string& name,
        const XMFLOAT3& fallback,
        std::uint32_t depth = 0)
    {
        if (depth >= 64u)
        {
            Warn("Unsupported texture graph", name + " (cycle or excessive depth)");
            return fallback;
        }
        const Parameter* parameter = FindParameter(parameters, name);
        if (!parameter || parameter->type != "texture" || parameter->strings.empty()) return ColorOr(parameters, name, fallback);
        const auto found = m_textures.find(parameter->strings.front());
        if (found == m_textures.end()) return fallback;
        if (found->second.implementation == "constant") return ColorOr(found->second.parameters, "value", fallback);
        if (found->second.implementation == "imagemap") return XMFLOAT3(1.0f, 1.0f, 1.0f);
        if (found->second.implementation == "scale")
        {
            const XMFLOAT3 tex = TextureColorOr(found->second.parameters, "tex", XMFLOAT3(1.0f, 1.0f, 1.0f), depth + 1u);
            const XMFLOAT3 scale = TextureColorOr(found->second.parameters, "scale", XMFLOAT3(1.0f, 1.0f, 1.0f), depth + 1u);
            return XMFLOAT3(tex.x * scale.x, tex.y * scale.y, tex.z * scale.z);
        }
        if (found->second.implementation == "mix")
        {
            const XMFLOAT3 first = TextureColorOr(found->second.parameters, "tex1", fallback, depth + 1u);
            const XMFLOAT3 second = TextureColorOr(found->second.parameters, "tex2", fallback, depth + 1u);
            const float amount = std::clamp(TextureFloatOr(found->second.parameters, "amount", 0.5f, depth + 1u), 0.0f, 1.0f);
            return XMFLOAT3(
                first.x + (second.x - first.x) * amount,
                first.y + (second.y - first.y) * amount,
                first.z + (second.z - first.z) * amount);
        }
        Warn("Unsupported texture graph", parameter->strings.front() + " (" + found->second.implementation + ")");
        return fallback;
    }

    float TextureFloatOr(
        const ParameterMap& parameters,
        const std::string& name,
        float fallback,
        std::uint32_t depth = 0)
    {
        if (depth >= 64u)
        {
            Warn("Unsupported texture graph", name + " (cycle or excessive depth)");
            return fallback;
        }
        const Parameter* parameter = FindParameter(parameters, name);
        if (!parameter) return fallback;
        if (!parameter->numbers.empty()) return static_cast<float>(parameter->numbers.front());
        if (parameter->type == "texture" && !parameter->strings.empty())
        {
            const auto found = m_textures.find(parameter->strings.front());
            if (found != m_textures.end() && found->second.implementation == "constant")
            {
                return static_cast<float>(NumberOr(found->second.parameters, "value", fallback));
            }
            if (found != m_textures.end() && found->second.implementation == "scale")
            {
                return TextureFloatOr(found->second.parameters, "tex", 1.0f, depth + 1u) *
                    TextureFloatOr(found->second.parameters, "scale", 1.0f, depth + 1u);
            }
            if (found != m_textures.end() && found->second.implementation == "mix")
            {
                const float first = TextureFloatOr(found->second.parameters, "tex1", fallback, depth + 1u);
                const float second = TextureFloatOr(found->second.parameters, "tex2", fallback, depth + 1u);
                const float amount = std::clamp(TextureFloatOr(found->second.parameters, "amount", 0.5f, depth + 1u), 0.0f, 1.0f);
                return first + (second - first) * amount;
            }
        }
        return fallback;
    }

    std::uint32_t CreateMaterial(
        const std::string& name,
        const std::string& typeValue,
        const ParameterMap& parameters,
        const std::filesystem::path& sourceDirectory)
    {
        const std::string type = Lower(typeValue);
        rb::SceneMaterial material;
        material.assignment = { name, "LookDev PBR" };
        XMFLOAT3 baseColor(0.5f, 0.5f, 0.5f);
        float roughness = 0.8f;
        float metallic = 0.0f;

        if (type == "diffuse" || type == "coateddiffuse")
        {
            baseColor = TextureColorOr(parameters, "reflectance", XMFLOAT3(0.5f, 0.5f, 0.5f));
            roughness = TextureFloatOr(parameters, "roughness", type == "diffuse" ? 1.0f : 0.25f);
        }
        else if (type == "conductor" || type == "coatedconductor")
        {
            metallic = 1.0f;
            baseColor = TextureColorOr(parameters, "reflectance", XMFLOAT3(0.91f, 0.92f, 0.92f));
            if (!FindParameter(parameters, "reflectance"))
            {
                const std::optional<XMFLOAT3> namedF0 = NamedConductorF0(parameters);
                if (namedF0) baseColor = *namedF0;
                else
                {
                    const XMFLOAT3 eta = ColorOr(parameters, "eta", XMFLOAT3(0.2f, 0.9f, 1.1f));
                    const XMFLOAT3 k = ColorOr(parameters, "k", XMFLOAT3(3.9f, 2.5f, 2.1f));
                    const auto f0 = [](float etaValue, float kValue)
                    {
                        const float numerator = (etaValue - 1.0f) * (etaValue - 1.0f) + kValue * kValue;
                        const float denominator = (etaValue + 1.0f) * (etaValue + 1.0f) + kValue * kValue;
                        return denominator > 0.0f ? std::clamp(numerator / denominator, 0.0f, 1.0f) : 0.9f;
                    };
                    baseColor = XMFLOAT3(f0(eta.x, k.x), f0(eta.y, k.y), f0(eta.z, k.z));
                }
            }
            roughness = TextureFloatOr(parameters, "roughness", 0.1f);
        }
        else if (type == "dielectric" || type == "thindielectric")
        {
            baseColor = TextureColorOr(parameters, "reflectance", XMFLOAT3(1.0f, 1.0f, 1.0f));
            roughness = TextureFloatOr(parameters, "roughness", 0.05f);
            const XMFLOAT3 transmittance = TextureColorOr(parameters, "transmittance", XMFLOAT3(1.0f, 1.0f, 1.0f));
            material.transmissionFactor = std::clamp(
                (std::max)({ transmittance.x, transmittance.y, transmittance.z }), 0.0f, 1.0f);
            material.indexOfRefraction = std::clamp(
                static_cast<float>(NumberOr(parameters, "eta", NumberOr(parameters, "index", 1.5))),
                1.0001f,
                3.0f);
            material.thinDielectric = type == "thindielectric";
            // The path tracer treats these as delta dielectric interfaces.
            // Keep the metal-rough fallback non-metallic for AOV consumers;
            // reflection/refraction is selected explicitly in RayGen.
            metallic = 0.0f;
            if (FindParameter(parameters, "roughness") && roughness > 1.0e-3f)
            {
                Warn("Rough dielectric is rendered as a smooth interface", name);
            }
            const float minTransmittance = (std::min)({ transmittance.x, transmittance.y, transmittance.z });
            const float maxTransmittance = (std::max)({ transmittance.x, transmittance.y, transmittance.z });
            if (maxTransmittance - minTransmittance > 1.0e-4f)
            {
                Warn("Colored dielectric transmittance is reduced to a scalar", name);
            }
        }
        else if (type == "diffusetransmission")
        {
            baseColor = TextureColorOr(parameters, "reflectance", TextureColorOr(parameters, "transmittance", XMFLOAT3(0.5f, 0.5f, 0.5f)));
            roughness = 1.0f;
            Warn("Opaque approximation for transmissive material", name);
        }
        else if (type == "mix")
        {
            const Parameter* materials = FindParameter(parameters, "materials");
            const float amount = std::clamp(TextureFloatOr(parameters, "amount", 0.5f), 0.0f, 1.0f);
            if (materials && materials->strings.size() >= 2)
            {
                const auto firstFound = m_namedMaterials.find(materials->strings[0]);
                const auto secondFound = m_namedMaterials.find(materials->strings[1]);
                if (firstFound != m_namedMaterials.end() && secondFound != m_namedMaterials.end() &&
                    firstFound->second < m_scene.materials.size() && secondFound->second < m_scene.materials.size())
                {
                    const rb::SceneMaterial& first = m_scene.materials[firstFound->second];
                    const rb::SceneMaterial& second = m_scene.materials[secondFound->second];
                    rb::SceneMaterial mixed = first;
                    mixed.assignment.materialName = name;
                    const auto blend = [amount](float a, float b) { return a + (b - a) * amount; };
                    for (std::size_t component = 0; component < 4; ++component)
                    {
                        mixed.assignment.baseColorFactor[component] = blend(
                            first.assignment.baseColorFactor[component], second.assignment.baseColorFactor[component]);
                        mixed.assignment.emissiveFactor[component] = blend(
                            first.assignment.emissiveFactor[component], second.assignment.emissiveFactor[component]);
                    }
                    mixed.baseColorFactor = XMFLOAT4(
                        mixed.assignment.baseColorFactor[0], mixed.assignment.baseColorFactor[1],
                        mixed.assignment.baseColorFactor[2], mixed.assignment.baseColorFactor[3]);
                    mixed.emissiveFactor = XMFLOAT4(
                        mixed.assignment.emissiveFactor[0], mixed.assignment.emissiveFactor[1],
                        mixed.assignment.emissiveFactor[2], mixed.assignment.emissiveFactor[3]);
                    mixed.assignment.roughnessFactor = blend(first.assignment.roughnessFactor, second.assignment.roughnessFactor);
                    mixed.assignment.metallicFactor = blend(first.assignment.metallicFactor, second.assignment.metallicFactor);
                    mixed.assignment.normalStrength = blend(first.assignment.normalStrength, second.assignment.normalStrength);
                    mixed.assignment.occlusionStrength = blend(first.assignment.occlusionStrength, second.assignment.occlusionStrength);
                    mixed.assignment.alphaCutoff = blend(first.assignment.alphaCutoff, second.assignment.alphaCutoff);
                    mixed.assignment.alphaMode = amount <= 0.0f ? first.assignment.alphaMode
                        : amount >= 1.0f ? second.assignment.alphaMode
                        : (first.assignment.alphaMode == rb::AlphaMode::Mask || second.assignment.alphaMode == rb::AlphaMode::Mask
                            ? rb::AlphaMode::Mask : rb::AlphaMode::Opaque);
                    mixed.twoSidedEmission = first.twoSidedEmission || second.twoSidedEmission;
                    mixed.transmissionFactor = blend(first.transmissionFactor, second.transmissionFactor);
                    mixed.indexOfRefraction = blend(first.indexOfRefraction, second.indexOfRefraction);
                    mixed.thinDielectric = amount < 0.5f ? first.thinDielectric : second.thinDielectric;
                    auto keepSharedTexture = [&](std::wstring& path, bool& present, const std::wstring& a, const std::wstring& b)
                    {
                        if (a == b) { path = a; present = !path.empty(); }
                        else { path.clear(); present = false; }
                    };
                    keepSharedTexture(mixed.baseColorTexturePath, mixed.hasBaseColorTexture, first.baseColorTexturePath, second.baseColorTexturePath);
                    keepSharedTexture(mixed.normalTexturePath, mixed.hasNormalTexture, first.normalTexturePath, second.normalTexturePath);
                    keepSharedTexture(mixed.roughnessTexturePath, mixed.hasRoughnessTexture, first.roughnessTexturePath, second.roughnessTexturePath);
                    keepSharedTexture(mixed.metallicTexturePath, mixed.hasMetallicTexture, first.metallicTexturePath, second.metallicTexturePath);
                    keepSharedTexture(mixed.occlusionTexturePath, mixed.hasOcclusionTexture, first.occlusionTexturePath, second.occlusionTexturePath);
                    keepSharedTexture(mixed.emissiveTexturePath, mixed.hasEmissiveTexture, first.emissiveTexturePath, second.emissiveTexturePath);
                    keepSharedTexture(mixed.alphaTexturePath, mixed.hasAlphaTexture, first.alphaTexturePath, second.alphaTexturePath);
                    if (amount > 0.0f && amount < 1.0f &&
                        (first.baseColorTexturePath != second.baseColorTexturePath || first.roughnessTexturePath != second.roughnessTexturePath))
                    {
                        Warn("Mixed material texture graphs reduced to blended constants", name);
                    }
                    const std::uint32_t index = static_cast<std::uint32_t>(m_scene.materials.size());
                    m_scene.materials.push_back(std::move(mixed));
                    return index;
                }
            }
            Warn("Fallback approximation for mix material", name);
        }
        else if (type == "interface")
        {
            Warn("Volume interface material is ignored", name);
        }
        else
        {
            Warn("Fallback approximation for unsupported material", name + " (" + type + ")");
        }

        roughness = std::clamp(roughness, 0.02f, 1.0f);
        material.baseColorFactor = XMFLOAT4(baseColor.x, baseColor.y, baseColor.z, 1.0f);
        material.assignment.baseColorFactor = { baseColor.x, baseColor.y, baseColor.z, 1.0f };
        material.assignment.roughnessFactor = roughness;
        material.assignment.metallicFactor = metallic;
        material.baseColorTexturePath = TexturePathFromParameter(parameters, "reflectance", sourceDirectory);
        if (material.baseColorTexturePath.empty()) material.baseColorTexturePath = TexturePathFromParameter(parameters, "transmittance", sourceDirectory);
        material.roughnessTexturePath = TexturePathFromParameter(parameters, "roughness", sourceDirectory);
        material.normalTexturePath = TexturePathFromParameter(parameters, "normalmap", sourceDirectory);
        material.alphaTexturePath = TexturePathFromParameter(parameters, "alpha", sourceDirectory);
        bool hasUvTransform = false;
        for (const char* parameterName : { "reflectance", "transmittance", "roughness", "normalmap", "alpha" })
        {
            const std::optional<XMFLOAT4> transform = TextureUvTransformFromParameter(parameters, parameterName);
            if (!transform) continue;
            if (!hasUvTransform)
            {
                material.uvScaleOffset = *transform;
                hasUvTransform = true;
            }
            else if (std::abs(material.uvScaleOffset.x - transform->x) > 1.0e-6f ||
                std::abs(material.uvScaleOffset.y - transform->y) > 1.0e-6f ||
                std::abs(material.uvScaleOffset.z - transform->z) > 1.0e-6f ||
                std::abs(material.uvScaleOffset.w - transform->w) > 1.0e-6f)
            {
                Warn("Conflicting material texture UV transforms reduced to the first transform", name);
            }
        }
        if (FindParameter(parameters, "displacement"))
        {
            Warn("Displacement is unsupported and was ignored", name);
        }
        material.hasBaseColorTexture = !material.baseColorTexturePath.empty();
        material.hasRoughnessTexture = !material.roughnessTexturePath.empty();
        material.hasNormalTexture = !material.normalTexturePath.empty();
        material.hasAlphaTexture = !material.alphaTexturePath.empty();
        if (material.hasAlphaTexture)
        {
            material.assignment.alphaMode = rb::AlphaMode::Mask;
            material.assignment.alphaCutoff = 0.5f;
        }
        const std::uint32_t index = static_cast<std::uint32_t>(m_scene.materials.size());
        m_scene.materials.push_back(std::move(material));
        return index;
    }

    std::uint32_t MaterialWithAreaEmission(std::uint32_t baseIndex, const AreaLightState& area)
    {
        if (!area.enabled) return baseIndex;
        std::ostringstream keyStream;
        keyStream << baseIndex << ':' << area.radiance.x << ':' << area.radiance.y << ':' << area.radiance.z << ':' << area.twoSided;
        const std::string key = keyStream.str();
        const auto found = m_emissiveMaterialCache.find(key);
        if (found != m_emissiveMaterialCache.end()) return found->second;
        rb::SceneMaterial material = m_scene.materials.at(baseIndex);
        material.assignment.materialName += " [PBRT area light]";
        material.emissiveFactor = XMFLOAT4(area.radiance.x, area.radiance.y, area.radiance.z, 1.0f);
        material.assignment.emissiveFactor = { area.radiance.x, area.radiance.y, area.radiance.z, 1.0f };
        material.twoSidedEmission = area.twoSided;
        const std::uint32_t index = static_cast<std::uint32_t>(m_scene.materials.size());
        m_scene.materials.push_back(std::move(material));
        m_emissiveMaterialCache[key] = index;
        return index;
    }

    void ParseFile(const std::filesystem::path& authoredPath, std::size_t depth)
    {
        if (depth >= MaxIncludeDepth) throw PbrtError(SourceLocation{ authoredPath, 1, 1 }, "Include nesting exceeds 64 files.");
        const std::filesystem::path path = CanonicalPath(authoredPath);
        if (!std::filesystem::exists(path)) throw PbrtError(SourceLocation{ path, 1, 1 }, "Included PBRT file was not found.");
        const std::wstring key = Lower(WideToUtf8(path.wstring())).empty() ? path.wstring() : Utf8ToWide(Lower(WideToUtf8(path.wstring())));
        if (!m_includeStack.insert(key).second) throw PbrtError(SourceLocation{ path, 1, 1 }, "Include cycle detected.");

        const std::vector<Token> tokens = Lexer(path, ReadFileUtf8(path)).Tokenize();
        std::size_t cursor = 0;
        while (cursor < tokens.size() && tokens[cursor].kind != TokenKind::End)
        {
            CheckCancelled(tokens[cursor].location);
            ParseStatement(tokens, cursor, path.parent_path(), depth);
        }
        m_includeStack.erase(key);
    }

    void ApplyTransform(FXMMATRIX operation)
    {
        m_state.transform = StoreMatrix(operation * LoadMatrix(m_state.transform));
    }

    XMMATRIX ReadAuthoredMatrix(const std::vector<Token>& tokens, std::size_t& cursor)
    {
        Require(tokens, cursor, TokenKind::LeftBracket, "'[' before a transform matrix");
        std::array<float, 16> values{};
        for (float& value : values) value = static_cast<float>(RequireNumber(tokens, cursor));
        Require(tokens, cursor, TokenKind::RightBracket, "']' after a transform matrix");
        // PBRT stores a column-vector matrix in row-major order. The renderer
        // uses DirectX row vectors, therefore the authored matrix is transposed.
        return XMMatrixTranspose(XMMatrixSet(
            values[0], values[1], values[2], values[3],
            values[4], values[5], values[6], values[7],
            values[8], values[9], values[10], values[11],
            values[12], values[13], values[14], values[15]));
    }

    void ParseStatement(
        const std::vector<Token>& tokens,
        std::size_t& cursor,
        const std::filesystem::path& sourceDirectory,
        std::size_t includeDepth)
    {
        const Token directiveToken = Require(tokens, cursor, TokenKind::Identifier, "a PBRT directive");
        const std::string directive = Lower(directiveToken.text);
        if (directive == "include")
        {
            const std::string included = RequireName(tokens, cursor, "an Include path");
            ParseFile(ResolveAssetPath(sourceDirectory, included), includeDepth + 1);
        }
        else if (directive == "worldbegin")
        {
            m_state = {};
            m_state.transform = StoreMatrix(XMMatrixIdentity());
            m_coordinateSystems["world"] = m_state.transform;
        }
        else if (directive == "attributebegin")
        {
            m_attributeStack.push_back(m_state);
        }
        else if (directive == "attributeend")
        {
            if (m_attributeStack.empty()) throw PbrtError(directiveToken.location, "AttributeEnd has no matching AttributeBegin.");
            m_state = m_attributeStack.back();
            m_attributeStack.pop_back();
        }
        else if (directive == "transformbegin")
        {
            m_transformStack.push_back(m_state.transform);
        }
        else if (directive == "transformend")
        {
            if (m_transformStack.empty()) throw PbrtError(directiveToken.location, "TransformEnd has no matching TransformBegin.");
            m_state.transform = m_transformStack.back();
            m_transformStack.pop_back();
        }
        else if (directive == "identity")
        {
            m_state.transform = StoreMatrix(XMMatrixIdentity());
        }
        else if (directive == "translate")
        {
            const float x = static_cast<float>(RequireNumber(tokens, cursor));
            const float y = static_cast<float>(RequireNumber(tokens, cursor));
            const float z = static_cast<float>(RequireNumber(tokens, cursor));
            ApplyTransform(XMMatrixTranslation(x, y, z));
        }
        else if (directive == "scale")
        {
            const float x = static_cast<float>(RequireNumber(tokens, cursor));
            const float y = static_cast<float>(RequireNumber(tokens, cursor));
            const float z = static_cast<float>(RequireNumber(tokens, cursor));
            ApplyTransform(XMMatrixScaling(x, y, z));
        }
        else if (directive == "rotate")
        {
            const float angle = XMConvertToRadians(static_cast<float>(RequireNumber(tokens, cursor)));
            const float x = static_cast<float>(RequireNumber(tokens, cursor));
            const float y = static_cast<float>(RequireNumber(tokens, cursor));
            const float z = static_cast<float>(RequireNumber(tokens, cursor));
            const XMVECTOR axis = XMVectorSet(x, y, z, 0.0f);
            if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-16f) throw PbrtError(directiveToken.location, "Rotate axis must be non-zero.");
            ApplyTransform(XMMatrixRotationAxis(axis, angle));
        }
        else if (directive == "lookat")
        {
            std::array<float, 9> values{};
            for (float& value : values) value = static_cast<float>(RequireNumber(tokens, cursor));
            const XMVECTOR position = XMVectorSet(values[0], values[1], values[2], 1.0f);
            const XMVECTOR target = XMVectorSet(values[3], values[4], values[5], 1.0f);
            const XMVECTOR up = XMVectorSet(values[6], values[7], values[8], 0.0f);
            XMVECTOR forward = XMVector3Normalize(target - position);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));
            XMVECTOR correctedUp = XMVector3Cross(forward, right);
            XMFLOAT3 p, r, u, f;
            XMStoreFloat3(&p, position); XMStoreFloat3(&r, right); XMStoreFloat3(&u, correctedUp); XMStoreFloat3(&f, forward);
            // The file-format CTM at Camera is world-to-camera. LookAt is
            // authored as an eye pose, so concatenate its inverse here.
            const XMMATRIX cameraToWorld = XMMatrixSet(
                r.x, r.y, r.z, 0.0f,
                u.x, u.y, u.z, 0.0f,
                f.x, f.y, f.z, 0.0f,
                p.x, p.y, p.z, 1.0f);
            ApplyTransform(XMMatrixInverse(nullptr, cameraToWorld));
        }
        else if (directive == "transform")
        {
            m_state.transform = StoreMatrix(ReadAuthoredMatrix(tokens, cursor));
        }
        else if (directive == "concattransform")
        {
            ApplyTransform(ReadAuthoredMatrix(tokens, cursor));
        }
        else if (directive == "coordinatesystem")
        {
            m_coordinateSystems[RequireName(tokens, cursor, "a coordinate-system name")] = m_state.transform;
        }
        else if (directive == "coordsystransform")
        {
            const std::string name = RequireName(tokens, cursor, "a coordinate-system name");
            const auto found = m_coordinateSystems.find(name);
            if (found == m_coordinateSystems.end()) throw PbrtError(directiveToken.location, "Unknown coordinate system '" + name + "'.");
            m_state.transform = found->second;
        }
        else if (directive == "reverseorientation")
        {
            m_state.reverseOrientation = !m_state.reverseOrientation;
        }
        else if (directive == "texture")
        {
            const std::string name = RequireName(tokens, cursor, "a texture name");
            TextureDefinition texture;
            texture.valueType = Lower(RequireName(tokens, cursor, "a texture value type"));
            texture.implementation = Lower(RequireName(tokens, cursor, "a texture implementation"));
            texture.parameters = ReadParameters(tokens, cursor);
            texture.sourceDirectory = sourceDirectory;
            if (texture.implementation != "imagemap" && texture.implementation != "constant" &&
                texture.implementation != "scale" && texture.implementation != "mix")
            {
                Warn("Unsupported procedural texture", name + " (" + texture.implementation + ")");
            }
            m_textures[name] = std::move(texture);
        }
        else if (directive == "makenamedmaterial")
        {
            const std::string name = RequireName(tokens, cursor, "a named material name");
            const ParameterMap parameters = ReadParameters(tokens, cursor);
            const std::string type = StringOr(parameters, "type", "diffuse");
            m_namedMaterials[name] = CreateMaterial(name, type, parameters, sourceDirectory);
        }
        else if (directive == "namedmaterial")
        {
            const std::string name = RequireName(tokens, cursor, "a named material name");
            const auto found = m_namedMaterials.find(name);
            if (found == m_namedMaterials.end())
            {
                Warn("Missing named material", name);
                m_state.materialIndex = 0;
            }
            else m_state.materialIndex = found->second;
        }
        else if (directive == "material")
        {
            const std::string type = RequireName(tokens, cursor, "a material type");
            const ParameterMap parameters = ReadParameters(tokens, cursor);
            m_state.materialIndex = CreateMaterial("Inline " + type + " " + std::to_string(m_scene.materials.size()), type, parameters, sourceDirectory);
        }
        else if (directive == "arealightsource")
        {
            const std::string type = Lower(RequireName(tokens, cursor, "an area-light type"));
            const ParameterMap parameters = ReadParameters(tokens, cursor);
            if (type == "diffuse")
            {
                XMFLOAT3 radiance = ColorOr(parameters, "l", XMFLOAT3(1.0f, 1.0f, 1.0f));
                const float scale = static_cast<float>(NumberOr(parameters, "scale", 1.0));
                radiance.x *= scale; radiance.y *= scale; radiance.z *= scale;
                m_state.areaLight = { true, radiance, BoolOr(parameters, "twosided", false) };
                m_scene.hasAuthoredLighting = true;
            }
            else
            {
                m_state.areaLight = {};
                Warn("Unsupported area-light type", type);
            }
        }
        else if (directive == "lightsource")
        {
            const std::string type = RequireName(tokens, cursor, "a light type");
            const ParameterMap parameters = ReadParameters(tokens, cursor);
            ParseLight(type, parameters, sourceDirectory, directiveToken.location);
        }
        else if (directive == "camera")
        {
            const std::string type = RequireName(tokens, cursor, "a camera type");
            const ParameterMap parameters = ReadParameters(tokens, cursor);
            ParseCamera(type, parameters, directiveToken.location);
        }
        else if (directive == "film")
        {
            const std::string type = RequireName(tokens, cursor, "a film type");
            const ParameterMap parameters = ReadParameters(tokens, cursor);
            (void)parameters;
            if (Lower(type) != "rgb") Warn("Unsupported Film implementation", type);
        }
        else if (directive == "shape")
        {
            const std::string type = RequireName(tokens, cursor, "a shape type");
            const ParameterMap parameters = ReadParameters(tokens, cursor);
            ParseShape(type, parameters, sourceDirectory, directiveToken.location);
        }
        else if (directive == "objectbegin")
        {
            const std::string name = RequireName(tokens, cursor, "an object name");
            if (m_activeObject) throw PbrtError(directiveToken.location, "Nested ObjectBegin definitions are invalid.");
            m_activeObject = name;
            m_objects[name].clear();
            m_attributeStack.push_back(m_state);
        }
        else if (directive == "objectend")
        {
            if (!m_activeObject) throw PbrtError(directiveToken.location, "ObjectEnd has no matching ObjectBegin.");
            m_activeObject.reset();
            if (m_attributeStack.empty()) throw PbrtError(directiveToken.location, "Object state stack is corrupt.");
            m_state = m_attributeStack.back();
            m_attributeStack.pop_back();
        }
        else if (directive == "objectinstance")
        {
            ObjectElement element;
            element.name = RequireName(tokens, cursor, "an object name");
            element.transform = m_state.transform;
            element.location = directiveToken.location;
            AppendElement(element);
        }
        else if (directive == "activetransform")
        {
            const std::string selection = Lower(RequireName(tokens, cursor, "an active transform selection"));
            if (selection != "all") Warn("Motion transform reduced to its static transform", selection);
        }
        else if (directive == "transformtimes")
        {
            RequireNumber(tokens, cursor); RequireNumber(tokens, cursor);
            Warn("Motion transform times are ignored");
        }
        else if (directive == "colorspace")
        {
            const std::string colorSpace = Lower(RequireName(tokens, cursor, "a color-space name"));
            if (colorSpace != "srgb" && colorSpace != "rec2020") Warn("Unsupported color space approximated as linear sRGB", colorSpace);
        }
        else if (directive == "mediuminterface")
        {
            RequireName(tokens, cursor, "an inside medium name");
            RequireName(tokens, cursor, "an outside medium name");
            Warn("Volume medium interface is ignored");
        }
        else if (directive == "makenamedmedium")
        {
            const std::string name = RequireName(tokens, cursor, "a medium name");
            ReadParameters(tokens, cursor);
            Warn("Volume medium is ignored", name);
        }
        else if (directive == "sampler" || directive == "integrator" || directive == "accelerator" ||
                 directive == "pixelfilter")
        {
            RequireName(tokens, cursor, "an implementation name");
            ReadParameters(tokens, cursor);
        }
        else if (directive == "option")
        {
            ReadParameters(tokens, cursor);
        }
        else if (directive == "attribute")
        {
            const std::string target = RequireName(tokens, cursor, "an Attribute target");
            ReadParameters(tokens, cursor);
            Warn("Renderer-specific Attribute is ignored", target);
        }
        else
        {
            throw PbrtError(directiveToken.location, "Unsupported or unknown PBRT directive '" + directiveToken.text + "'.");
        }
    }

    void ParseCamera(const std::string& typeValue, const ParameterMap& parameters, const SourceLocation& location)
    {
        const std::string type = Lower(typeValue);
        if (type != "perspective")
        {
            Warn("Unsupported camera type; scene framing will be used", type);
            return;
        }
        // PBRT's pre-WorldBegin CTM is world-to-camera, while shape/light CTMs
        // are object-to-world. The renderer camera state needs camera-to-world.
        const XMMATRIX cameraToWorld = XMMatrixInverse(nullptr, LoadMatrix(m_state.transform));
        rb::SceneCamera camera;
        camera.position = TransformPoint(XMFLOAT3(0.0f, 0.0f, 0.0f), cameraToWorld);
        camera.forward = TransformDirection(XMFLOAT3(0.0f, 0.0f, 1.0f), cameraToWorld);
        camera.up = TransformDirection(XMFLOAT3(0.0f, 1.0f, 0.0f), cameraToWorld);
        camera.fovDegrees = std::clamp(static_cast<float>(NumberOr(parameters, "fov", 60.0)), 1.0f, 179.0f);
        if (!std::isfinite(camera.fovDegrees)) throw PbrtError(location, "Camera FOV is non-finite.");
        m_scene.camera = camera;
        m_coordinateSystems["camera"] = StoreMatrix(cameraToWorld);
    }

    void ParseLight(
        const std::string& typeValue,
        const ParameterMap& parameters,
        const std::filesystem::path& sourceDirectory,
        const SourceLocation& location)
    {
        const std::string type = Lower(typeValue);
        XMFLOAT3 radiance = ColorOr(parameters, type == "point" || type == "spot" ? "i" : "l", XMFLOAT3(1.0f, 1.0f, 1.0f));
        const float scale = static_cast<float>(NumberOr(parameters, "scale", 1.0));
        radiance.x *= scale; radiance.y *= scale; radiance.z *= scale;
        if (type == "infinite")
        {
            m_scene.hasAuthoredLighting = true;
            if (m_scene.environment)
            {
                Warn("Additional infinite light is ignored", WideToUtf8(location.file.wstring()));
                return;
            }
            rb::SceneEnvironment environment;
            const std::string filename = StringOr(parameters, "filename");
            if (!filename.empty())
            {
                const std::filesystem::path resolved = ResolveAssetPath(sourceDirectory, filename);
                if (std::filesystem::exists(resolved)) environment.texturePath = resolved.wstring();
                else Warn("Missing optional texture", WideToUtf8(resolved.wstring()));
            }
            environment.scale = radiance;
            environment.lightToWorld = m_state.transform;
            environment.equalAreaMapping = true;
            m_scene.environment = environment;
            return;
        }

        rb::SceneLight light;
        light.radiance = radiance;
        const XMMATRIX transform = LoadMatrix(m_state.transform);
        const auto pointOr = [&](const char* name, const XMFLOAT3& fallback)
        {
            const Parameter* parameter = FindParameter(parameters, name);
            return parameter && parameter->numbers.size() >= 3
                ? XMFLOAT3(
                    static_cast<float>(parameter->numbers[0]),
                    static_cast<float>(parameter->numbers[1]),
                    static_cast<float>(parameter->numbers[2]))
                : fallback;
        };
        const XMFLOAT3 from = pointOr("from", XMFLOAT3(0.0f, 0.0f, 0.0f));
        const XMFLOAT3 to = pointOr("to", XMFLOAT3(0.0f, 0.0f, 1.0f));
        light.position = TransformPoint(from, transform);
        light.direction = TransformDirection(XMFLOAT3(to.x - from.x, to.y - from.y, to.z - from.z), transform);
        if (type == "point") light.type = rb::SceneLightType::Point;
        else if (type == "spot")
        {
            light.type = rb::SceneLightType::Spot;
            light.coneAngleDegrees = std::clamp(static_cast<float>(NumberOr(parameters, "coneangle", 30.0)), 0.01f, 179.0f);
            light.coneDeltaDegrees = std::clamp(static_cast<float>(NumberOr(parameters, "conedeltaangle", 5.0)), 0.0f, light.coneAngleDegrees);
        }
        else if (type == "distant")
        {
            light.type = rb::SceneLightType::Distant;
        }
        else
        {
            Warn("Unsupported analytic light type", type);
            return;
        }
        m_scene.hasAuthoredLighting = true;
        m_scene.lights.push_back(light);
    }

    std::uint32_t AddTriangleMesh(
        const ParameterMap& parameters,
        std::uint32_t materialIndex,
        bool reverseOrientation,
        const SourceLocation& location)
    {
        const Parameter* positions = FindParameter(parameters, "p");
        const Parameter* indices = FindParameter(parameters, "indices");
        if (!positions || positions->numbers.size() < 9 || positions->numbers.size() % 3 != 0)
            throw PbrtError(location, "trianglemesh requires a point3 P array.");
        if (!indices || indices->numbers.size() < 3 || indices->numbers.size() % 3 != 0)
            throw PbrtError(location, "trianglemesh requires a triangular integer indices array.");

        rb::SceneMesh mesh;
        mesh.name = "trianglemesh " + std::to_string(m_scene.meshes.size());
        InitializeMeshBounds(mesh);
        const std::size_t vertexCount = positions->numbers.size() / 3;
        mesh.vertices.resize(vertexCount);
        const Parameter* normals = FindParameter(parameters, "n");
        const bool normalsPresent = normals && normals->numbers.size() == positions->numbers.size();
        const Parameter* texcoords = FindParameter(parameters, "uv");
        if (!texcoords) texcoords = FindParameter(parameters, "st");
        const bool texcoordsPresent = texcoords && texcoords->numbers.size() >= vertexCount * 2;
        for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
        {
            rb::SceneVertex& vertex = mesh.vertices[vertexIndex];
            vertex.position = XMFLOAT3(
                static_cast<float>(positions->numbers[vertexIndex * 3]),
                static_cast<float>(positions->numbers[vertexIndex * 3 + 1]),
                static_cast<float>(positions->numbers[vertexIndex * 3 + 2]));
            vertex.normal = normalsPresent
                ? XMFLOAT3(
                    static_cast<float>(normals->numbers[vertexIndex * 3]),
                    static_cast<float>(normals->numbers[vertexIndex * 3 + 1]),
                    static_cast<float>(normals->numbers[vertexIndex * 3 + 2]))
                : XMFLOAT3(0.0f, 0.0f, 0.0f);
            vertex.texcoord = texcoordsPresent
                ? XMFLOAT2(static_cast<float>(texcoords->numbers[vertexIndex * 2]), static_cast<float>(texcoords->numbers[vertexIndex * 2 + 1]))
                : XMFLOAT2(0.0f, 0.0f);
            ExpandBounds(mesh.boundsMin, mesh.boundsMax, vertex.position);
        }
        mesh.indices.reserve(indices->numbers.size());
        for (double indexValue : indices->numbers)
        {
            if (indexValue < 0.0 || indexValue >= static_cast<double>(vertexCount) || std::floor(indexValue) != indexValue)
                throw PbrtError(indices->location, "trianglemesh index is outside the vertex array.");
            mesh.indices.push_back(static_cast<std::uint32_t>(indexValue));
        }
        if (reverseOrientation)
        {
            for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
            for (rb::SceneVertex& vertex : mesh.vertices)
            {
                vertex.normal.x = -vertex.normal.x; vertex.normal.y = -vertex.normal.y; vertex.normal.z = -vertex.normal.z;
            }
        }
        GenerateNormalsAndTangents(mesh, normalsPresent);
        mesh.draws.push_back({ static_cast<std::uint32_t>(mesh.indices.size()), 0, 0, materialIndex });
        const std::uint32_t meshIndex = static_cast<std::uint32_t>(m_scene.meshes.size());
        m_scene.meshes.push_back(std::move(mesh));
        return meshIndex;
    }

    std::uint32_t LoadPlyMesh(
        const std::filesystem::path& path,
        std::uint32_t materialIndex,
        bool reverseOrientation,
        const SourceLocation& location)
    {
        if (!std::filesystem::exists(path)) throw PbrtError(location, "PLY file was not found: " + WideToUtf8(path.wstring()));
        if (Lower(path.extension().string()) == ".gz") throw PbrtError(location, ".ply.gz is outside the v1 compatibility scope.");
        std::ostringstream cacheKeyStream;
        cacheKeyStream << Lower(WideToUtf8(path.wstring())) << ':' << materialIndex << ':' << reverseOrientation;
        const std::string cacheKey = cacheKeyStream.str();
        const auto cached = m_plyMeshCache.find(cacheKey);
        if (cached != m_plyMeshCache.end()) return cached->second;

        ReportProgress(rb::SceneImportProgress::Stage::LoadingAssets, ++m_assetsLoaded, 0, path.wstring());
        CheckCancelled(location);
        Assimp::Importer importer;
        const aiScene* source = importer.ReadFile(
            WideToUtf8(path.wstring()),
            aiProcess_Triangulate | aiProcess_CalcTangentSpace | aiProcess_GenSmoothNormals |
            aiProcess_ImproveCacheLocality | aiProcess_SortByPType);
        if (!source || source->mNumMeshes == 0)
        {
            const std::string error = importer.GetErrorString();
            throw PbrtError(location, "Assimp failed to read PLY '" + WideToUtf8(path.wstring()) + "': " + error);
        }

        rb::SceneMesh mesh;
        mesh.name = WideToUtf8(path.filename().wstring());
        InitializeMeshBounds(mesh);
        for (std::uint32_t sourceMeshIndex = 0; sourceMeshIndex < source->mNumMeshes; ++sourceMeshIndex)
        {
            CheckCancelled(location);
            const aiMesh* sourceMesh = source->mMeshes[sourceMeshIndex];
            const std::uint32_t baseVertex = static_cast<std::uint32_t>(mesh.vertices.size());
            const std::uint32_t startIndex = static_cast<std::uint32_t>(mesh.indices.size());
            if (sourceMesh->mNumVertices > (std::numeric_limits<std::uint32_t>::max)() - baseVertex)
                throw PbrtError(location, "PLY vertex count exceeds 32-bit indexing.");
            for (std::uint32_t vertexIndex = 0; vertexIndex < sourceMesh->mNumVertices; ++vertexIndex)
            {
                const aiVector3D p = sourceMesh->mVertices[vertexIndex];
                const aiVector3D n = sourceMesh->HasNormals() ? sourceMesh->mNormals[vertexIndex] : aiVector3D(0.0f, 0.0f, 0.0f);
                const aiVector3D uv = sourceMesh->HasTextureCoords(0) ? sourceMesh->mTextureCoords[0][vertexIndex] : aiVector3D(0.0f, 0.0f, 0.0f);
                const aiVector3D t = sourceMesh->HasTangentsAndBitangents() ? sourceMesh->mTangents[vertexIndex] : aiVector3D(1.0f, 0.0f, 0.0f);
                rb::SceneVertex vertex;
                vertex.position = XMFLOAT3(p.x, p.y, p.z);
                vertex.normal = XMFLOAT3(n.x, n.y, n.z);
                vertex.texcoord = XMFLOAT2(uv.x, uv.y);
                vertex.tangent = XMFLOAT4(t.x, t.y, t.z, 1.0f);
                mesh.vertices.push_back(vertex);
                ExpandBounds(mesh.boundsMin, mesh.boundsMax, vertex.position);
            }
            for (std::uint32_t faceIndex = 0; faceIndex < sourceMesh->mNumFaces; ++faceIndex)
            {
                const aiFace& face = sourceMesh->mFaces[faceIndex];
                if (face.mNumIndices != 3) continue;
                mesh.indices.push_back(baseVertex + face.mIndices[0]);
                mesh.indices.push_back(baseVertex + face.mIndices[reverseOrientation ? 2 : 1]);
                mesh.indices.push_back(baseVertex + face.mIndices[reverseOrientation ? 1 : 2]);
            }
            const std::uint32_t indexCount = static_cast<std::uint32_t>(mesh.indices.size()) - startIndex;
            if (indexCount > 0) mesh.draws.push_back({ indexCount, startIndex, 0, materialIndex });
        }
        if (mesh.vertices.empty() || mesh.indices.empty()) throw PbrtError(location, "PLY contained no renderable triangles.");
        if (reverseOrientation)
        {
            for (rb::SceneVertex& vertex : mesh.vertices)
            {
                vertex.normal.x = -vertex.normal.x; vertex.normal.y = -vertex.normal.y; vertex.normal.z = -vertex.normal.z;
            }
        }
        const bool normalsPresent = source->mMeshes[0]->HasNormals();
        GenerateNormalsAndTangents(mesh, normalsPresent);
        const std::uint32_t meshIndex = static_cast<std::uint32_t>(m_scene.meshes.size());
        m_scene.meshes.push_back(std::move(mesh));
        m_plyMeshCache[cacheKey] = meshIndex;
        return meshIndex;
    }

    std::uint32_t AddDiskMesh(
        const ParameterMap& parameters,
        std::uint32_t materialIndex,
        bool reverseOrientation,
        const SourceLocation& location)
    {
        const float radius = static_cast<float>(NumberOr(parameters, "radius", 1.0));
        const float innerRadius = static_cast<float>(NumberOr(parameters, "innerradius", 0.0));
        const float height = static_cast<float>(NumberOr(parameters, "height", 0.0));
        const float phiMax = std::clamp(static_cast<float>(NumberOr(parameters, "phimax", 360.0)), 0.0f, 360.0f);
        if (!std::isfinite(radius) || !std::isfinite(innerRadius) || !std::isfinite(height) ||
            radius <= 0.0f || innerRadius < 0.0f || innerRadius >= radius || phiMax <= 0.0f)
        {
            throw PbrtError(location, "disk has an invalid radius, inner radius, height, or phi range.");
        }

        constexpr float Pi = 3.14159265358979323846f;
        const std::uint32_t segments = (std::max)(3u, static_cast<std::uint32_t>(std::ceil(64.0f * phiMax / 360.0f)));
        rb::SceneMesh mesh;
        mesh.name = "disk " + std::to_string(m_scene.meshes.size());
        InitializeMeshBounds(mesh);
        const float normalZ = reverseOrientation ? -1.0f : 1.0f;
        const auto appendVertex = [&](float radial, float angle)
        {
            rb::SceneVertex vertex;
            vertex.position = XMFLOAT3(radial * std::cos(angle), radial * std::sin(angle), height);
            vertex.normal = XMFLOAT3(0.0f, 0.0f, normalZ);
            vertex.texcoord = XMFLOAT2(
                0.5f + 0.5f * vertex.position.x / radius,
                0.5f + 0.5f * vertex.position.y / radius);
            ExpandBounds(mesh.boundsMin, mesh.boundsMax, vertex.position);
            mesh.vertices.push_back(vertex);
        };

        if (innerRadius <= 1.0e-7f)
        {
            appendVertex(0.0f, 0.0f);
            for (std::uint32_t segment = 0; segment <= segments; ++segment)
            {
                const float angle = phiMax * Pi / 180.0f * static_cast<float>(segment) / static_cast<float>(segments);
                appendVertex(radius, angle);
            }
            for (std::uint32_t segment = 0; segment < segments; ++segment)
            {
                mesh.indices.push_back(0u);
                mesh.indices.push_back(segment + (reverseOrientation ? 2u : 1u));
                mesh.indices.push_back(segment + (reverseOrientation ? 1u : 2u));
            }
        }
        else
        {
            for (std::uint32_t segment = 0; segment <= segments; ++segment)
            {
                const float angle = phiMax * Pi / 180.0f * static_cast<float>(segment) / static_cast<float>(segments);
                appendVertex(innerRadius, angle);
                appendVertex(radius, angle);
            }
            for (std::uint32_t segment = 0; segment < segments; ++segment)
            {
                const std::uint32_t inner0 = segment * 2u;
                const std::uint32_t outer0 = inner0 + 1u;
                const std::uint32_t inner1 = inner0 + 2u;
                const std::uint32_t outer1 = inner0 + 3u;
                const std::array<std::uint32_t, 6> indices = reverseOrientation
                    ? std::array<std::uint32_t, 6>{ inner0, outer1, outer0, inner0, inner1, outer1 }
                    : std::array<std::uint32_t, 6>{ inner0, outer0, outer1, inner0, outer1, inner1 };
                mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
            }
        }
        GenerateNormalsAndTangents(mesh, true);
        mesh.draws.push_back({ static_cast<std::uint32_t>(mesh.indices.size()), 0, 0, materialIndex });
        const std::uint32_t meshIndex = static_cast<std::uint32_t>(m_scene.meshes.size());
        m_scene.meshes.push_back(std::move(mesh));
        return meshIndex;
    }

    void ParseShape(
        const std::string& typeValue,
        const ParameterMap& parameters,
        const std::filesystem::path& sourceDirectory,
        const SourceLocation& location)
    {
        const std::string type = Lower(typeValue);
        const float constantAlpha = TextureFloatOr(parameters, "alpha", 1.0f);
        if (constantAlpha <= 0.0f)
        {
            Warn("Fully transparent shape skipped", type);
            return;
        }
        std::uint32_t materialIndex = MaterialWithAreaEmission(m_state.materialIndex, m_state.areaLight);
        const std::wstring alphaTexture = TexturePathFromParameter(parameters, "alpha", sourceDirectory);
        if (!alphaTexture.empty() || constantAlpha < 0.999f)
        {
            rb::SceneMaterial material = m_scene.materials.at(materialIndex);
            material.assignment.materialName += " [shape alpha]";
            material.alphaTexturePath = alphaTexture;
            material.hasAlphaTexture = !alphaTexture.empty();
            const std::optional<XMFLOAT4> alphaUvTransform = TextureUvTransformFromParameter(parameters, "alpha");
            if (alphaUvTransform) material.uvScaleOffset = *alphaUvTransform;
            const float alphaFactor = std::clamp(constantAlpha, 0.0f, 1.0f);
            material.baseColorFactor.w *= alphaFactor;
            material.assignment.baseColorFactor[3] *= alphaFactor;
            material.assignment.alphaMode = rb::AlphaMode::Mask;
            material.assignment.alphaCutoff = 0.5f;
            materialIndex = static_cast<std::uint32_t>(m_scene.materials.size());
            m_scene.materials.push_back(std::move(material));
        }

        std::uint32_t meshIndex = 0;
        if (type == "trianglemesh")
        {
            meshIndex = AddTriangleMesh(parameters, materialIndex, m_state.reverseOrientation, location);
        }
        else if (type == "plymesh")
        {
            const std::string filename = StringOr(parameters, "filename");
            if (filename.empty()) throw PbrtError(location, "plymesh is missing its filename parameter.");
            meshIndex = LoadPlyMesh(ResolveAssetPath(sourceDirectory, filename), materialIndex, m_state.reverseOrientation, location);
        }
        else if (type == "disk")
        {
            meshIndex = AddDiskMesh(parameters, materialIndex, m_state.reverseOrientation, location);
        }
        else
        {
            Warn("Unsupported shape skipped", type);
            return;
        }
        AppendElement(MeshElement{ meshIndex, m_state.transform, location });
    }

    void AppendElement(ObjectDefinitionElement element)
    {
        if (m_activeObject) m_objects[*m_activeObject].push_back(std::move(element));
        else m_rootElements.push_back(std::move(element));
    }

    void ResolveElement(
        const ObjectDefinitionElement& element,
        FXMMATRIX parentTransform,
        std::vector<rb::SceneInstance>& destination,
        std::unordered_set<std::string>& objectStack)
    {
        if (const MeshElement* mesh = std::get_if<MeshElement>(&element))
        {
            const XMMATRIX world = LoadMatrix(mesh->transform) * parentTransform;
            const float determinant = XMVectorGetX(XMMatrixDeterminant(world));
            if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-10f)
            {
                throw PbrtError(mesh->location, "Shape instance has a singular or non-finite transform.");
            }
            rb::SceneInstance instance;
            instance.meshIndex = mesh->meshIndex;
            instance.transform = StoreMatrix(world);
            instance.normalTransform = StoreMatrix(XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
            destination.push_back(instance);
            return;
        }
        const ObjectElement& object = std::get<ObjectElement>(element);
        const auto found = m_objects.find(object.name);
        if (found == m_objects.end()) throw PbrtError(object.location, "ObjectInstance references undefined object '" + object.name + "'.");
        if (!objectStack.insert(object.name).second) throw PbrtError(object.location, "ObjectInstance cycle detected at '" + object.name + "'.");
        const XMMATRIX world = LoadMatrix(object.transform) * parentTransform;
        for (const ObjectDefinitionElement& child : found->second) ResolveElement(child, world, destination, objectStack);
        objectStack.erase(object.name);
    }

    void ResolveRootObjects()
    {
        ReportProgress(rb::SceneImportProgress::Stage::Finalizing, 0, m_rootElements.size(), m_rootPath.wstring());
        std::unordered_set<std::string> objectStack;
        for (std::size_t index = 0; index < m_rootElements.size(); ++index)
        {
            CheckCancelled(SourceLocation{ m_rootPath, 1, 1 });
            ResolveElement(m_rootElements[index], XMMatrixIdentity(), m_scene.instances, objectStack);
            ReportProgress(rb::SceneImportProgress::Stage::Finalizing, index + 1, m_rootElements.size(), m_rootPath.wstring());
        }
        if (m_scene.instances.size() >= (1u << 24u))
            throw PbrtError(SourceLocation{ m_rootPath, 1, 1 }, "Scene exceeds the DXR 24-bit instance index limit.");
    }

    void ComputeWorldBounds()
    {
        m_scene.boundsMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        m_scene.boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const rb::SceneInstance& instance : m_scene.instances)
        {
            if (instance.meshIndex >= m_scene.meshes.size()) continue;
            const rb::SceneMesh& mesh = m_scene.meshes[instance.meshIndex];
            const XMMATRIX transform = LoadMatrix(instance.transform);
            for (std::uint32_t corner = 0; corner < 8; ++corner)
            {
                const XMFLOAT3 local(
                    (corner & 1) ? mesh.boundsMax.x : mesh.boundsMin.x,
                    (corner & 2) ? mesh.boundsMax.y : mesh.boundsMin.y,
                    (corner & 4) ? mesh.boundsMax.z : mesh.boundsMin.z);
                ExpandBounds(m_scene.boundsMin, m_scene.boundsMax, TransformPoint(local, transform));
            }
        }
    }

    std::string BuildDiagnostics(const rb::ImportedScene& scene) const
    {
        std::uint64_t vertexCount = 0;
        std::uint64_t triangleCount = 0;
        for (const rb::SceneMesh& mesh : scene.meshes)
        {
            vertexCount += mesh.vertices.size();
            triangleCount += mesh.indices.size() / 3;
        }
        std::ostringstream stream;
        stream << "Loaded PBRT scene " << WideToUtf8(m_rootPath.filename().wstring())
               << " with " << scene.meshes.size() << " shared meshes, "
               << scene.instances.size() << " instances, " << vertexCount
               << " unique vertices, " << triangleCount << " unique triangles, "
               << scene.materials.size() << " materials, and " << scene.lights.size() << " analytic lights.";
        for (const auto& [category, bucket] : m_warnings)
        {
            stream << "\nWarning: " << category << " (" << bucket.count << ')';
            if (!bucket.examples.empty())
            {
                stream << ": ";
                for (std::size_t index = 0; index < bucket.examples.size(); ++index)
                {
                    if (index) stream << ", ";
                    stream << bucket.examples[index];
                }
            }
        }
        return stream.str();
    }

    std::filesystem::path m_rootPath;
    const std::atomic_bool* m_cancelRequested = nullptr;
    rb::SceneImportProgressCallback m_progress;
    rb::ImportedScene m_scene;
    GraphicsState m_state;
    std::vector<GraphicsState> m_attributeStack;
    std::vector<XMFLOAT4X4> m_transformStack;
    std::unordered_map<std::string, XMFLOAT4X4> m_coordinateSystems;
    std::unordered_map<std::string, TextureDefinition> m_textures;
    std::unordered_map<std::string, std::uint32_t> m_namedMaterials;
    std::unordered_map<std::string, std::uint32_t> m_emissiveMaterialCache;
    std::unordered_map<std::string, std::uint32_t> m_plyMeshCache;
    std::unordered_map<std::string, std::vector<ObjectDefinitionElement>> m_objects;
    std::vector<ObjectDefinitionElement> m_rootElements;
    std::optional<std::string> m_activeObject;
    std::set<std::wstring> m_includeStack;
    std::map<std::string, WarningBucket> m_warnings;
    std::uint64_t m_assetsLoaded = 0;
};
}

namespace rb
{
SceneImportResult ImportPbrtScene(
    const std::wstring& path,
    const std::atomic_bool* cancelRequested,
    const SceneImportProgressCallback& progress)
{
    return PbrtImporter(path, cancelRequested, progress).Run();
}
}

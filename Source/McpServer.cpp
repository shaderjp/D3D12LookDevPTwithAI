#include "stdafx.h"
#include "McpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <functional>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>

namespace
{
    constexpr const char* ModernProtocolVersion = "2026-07-28";
    constexpr const char* LegacyProtocolVersion = "2025-11-25";
    constexpr const char* CompatProtocolVersion = "2025-06-18";
    constexpr size_t MaxHttpBodyBytes = 16u * 1024u * 1024u;
    constexpr size_t MaxHttpHeaderBytes = 64u * 1024u;
    constexpr size_t MaxHttpChunks = 65536;
    constexpr size_t MaxConnections = 64;
    constexpr size_t MaxSubscriptions = 16;
    constexpr size_t MaxLegacySessions = 64;
    constexpr size_t MaxSubscriptionUris = 64;
    constexpr auto LegacySessionIdleTimeout = std::chrono::minutes(30);
    constexpr auto SubscriptionKeepAlive = std::chrono::seconds(15);
    constexpr int64_t CatalogTtlMs = 3600000;

    std::string ToLowerAscii(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
        return text;
    }

    bool StartsWith(const std::string& text, const std::string& prefix)
    {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    bool ContainsHeaderToken(const std::string& header, const std::string& token)
    {
        const std::string lowerHeader = ToLowerAscii(header);
        const std::string lowerToken = ToLowerAscii(token);
        return lowerHeader.find(lowerToken) != std::string::npos || lowerHeader.find("*/*") != std::string::npos;
    }

    std::string HeaderValue(const std::map<std::string, std::string>& headers, const std::string& name)
    {
        const auto it = headers.find(ToLowerAscii(name));
        return it == headers.end() ? std::string{} : it->second;
    }

    bool IsLegacyProtocolVersion(const std::string& version)
    {
        return version == LegacyProtocolVersion || version == CompatProtocolVersion;
    }

    bool IsSupportedProtocolVersion(const std::string& version)
    {
        return version == ModernProtocolVersion || IsLegacyProtocolVersion(version);
    }

    std::string JsonMemberString(const cld::JsonValue& value, const char* name)
    {
        return cld::JsonStringOr(value, name);
    }

    const cld::JsonValue* JsonMember(const cld::JsonValue& value, const char* name)
    {
        return cld::FindMember(value, name);
    }

    std::string JsonIdToJson(const cld::JsonValue& value)
    {
        const cld::JsonValue* id = JsonMember(value, "id");
        return id ? cld::JsonValueToJson(*id) : "null";
    }

    bool HasJsonId(const cld::JsonValue& value)
    {
        return JsonMember(value, "id") != nullptr;
    }

    std::string MakeResponse(const std::string& idJson, const std::string& resultJson)
    {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"result\":" + resultJson + "}";
    }

    std::string MakeError(const std::string& idJson, int code, const std::string& message)
    {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + cld::EscapeJson(message) + "\"}}";
    }

    std::string MakeErrorWithData(const std::string& idJson, int code, const std::string& message, const std::string& dataJson)
    {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + idJson + ",\"error\":{\"code\":" +
            std::to_string(code) + ",\"message\":\"" + cld::EscapeJson(message) + "\",\"data\":" + dataJson + "}}";
    }

    const char* ServerInfoJson()
    {
        return R"json({"name":"d3d12lookdevpt","title":"D3D12LookDevPT","version":"0.1.0"})json";
    }

    std::string ModernResultJson(const std::string& resultJson, int64_t ttlMs = -1)
    {
        std::string result = "{\"resultType\":\"complete\",\"_meta\":{\"io.modelcontextprotocol/serverInfo\":" +
            std::string(ServerInfoJson()) + "}";
        if (ttlMs >= 0)
        {
            result += ",\"ttlMs\":" + std::to_string(ttlMs) + ",\"cacheScope\":\"private\"";
        }
        if (resultJson.size() >= 2 && resultJson.front() == '{' && resultJson.back() == '}')
        {
            const std::string members = resultJson.substr(1, resultJson.size() - 2);
            if (!members.empty())
            {
                result += "," + members;
            }
        }
        result += "}";
        return result;
    }

    std::string ProtocolResponse(const std::string& idJson, const std::string& resultJson, bool modern, int64_t ttlMs = -1)
    {
        return MakeResponse(idJson, modern ? ModernResultJson(resultJson, ttlMs) : resultJson);
    }

    std::string SupportedVersionsJson()
    {
        return "[\"" + std::string(ModernProtocolVersion) + "\",\"" + LegacyProtocolVersion + "\",\"" + CompatProtocolVersion + "\"]";
    }

    std::string TextContentJson(const std::string& text)
    {
        return "[{\"type\":\"text\",\"text\":\"" + cld::EscapeJson(text) + "\"}]";
    }

    std::string ToolResultToJson(const mcp::ToolResult& result)
    {
        const std::string content = result.contentJson.empty() ? TextContentJson(result.text) : result.contentJson;
        std::string json = "{\"content\":" + content + ",\"structuredContent\":" + (result.structuredJson.empty() ? "{}" : result.structuredJson);
        if (result.isError)
        {
            json += ",\"isError\":true";
        }
        json += "}";
        return json;
    }

    std::string MakeHttpErrorBody(int code, const std::string& message)
    {
        return MakeError("null", code, message);
    }

    const char* HttpReasonPhrase(int status)
    {
        switch (status)
        {
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 501: return "Not Implemented";
        default: return "Bad Request";
        }
    }

    mcp::Server::HttpResponse JsonResponse(int status, const char* reason, const std::string& body)
    {
        mcp::Server::HttpResponse response;
        response.status = status;
        response.reason = reason;
        response.contentType = "application/json";
        response.body = body;
        return response;
    }

    std::string ToolJson(const char* name, const char* title, const char* description, const char* inputSchema, bool readOnly = false)
    {
        std::string json = "{\"name\":\"" + std::string(name) +
            "\",\"title\":\"" + cld::EscapeJson(title) +
            "\",\"description\":\"" + cld::EscapeJson(description) +
            "\",\"inputSchema\":" + inputSchema;
        if (readOnly)
        {
            json += ",\"annotations\":{\"readOnlyHint\":true}";
        }
        json += "}";
        return json;
    }

    std::string ResourceJson(const char* uri, const char* name, const char* title, const char* description, const char* mimeType)
    {
        return "{\"uri\":\"" + std::string(uri) +
            "\",\"name\":\"" + cld::EscapeJson(name) +
            "\",\"title\":\"" + cld::EscapeJson(title) +
            "\",\"description\":\"" + cld::EscapeJson(description) +
            "\",\"mimeType\":\"" + cld::EscapeJson(mimeType) + "\"}";
    }

    std::string ResourceTemplateJson(const char* uriTemplate, const char* name, const char* title, const char* description, const char* mimeType)
    {
        return "{\"uriTemplate\":\"" + std::string(uriTemplate) +
            "\",\"name\":\"" + cld::EscapeJson(name) +
            "\",\"title\":\"" + cld::EscapeJson(title) +
            "\",\"description\":\"" + cld::EscapeJson(description) +
            "\",\"mimeType\":\"" + cld::EscapeJson(mimeType) + "\"}";
    }

    std::string PromptJson(const char* name, const char* title, const char* description)
    {
        return "{\"name\":\"" + std::string(name) +
            "\",\"title\":\"" + cld::EscapeJson(title) +
            "\",\"description\":\"" + cld::EscapeJson(description) + "\"}";
    }

    std::string PromptTextResult(const char* description, const std::string& text)
    {
        return "{\"description\":\"" + cld::EscapeJson(description) +
            "\",\"messages\":[{\"role\":\"user\",\"content\":{\"type\":\"text\",\"text\":\"" +
            cld::EscapeJson(text) + "\"}}]}";
    }

    std::string EmptyObjectSchema()
    {
        return R"json({"type":"object","properties":{},"additionalProperties":false})json";
    }

    bool ValidateJsonSchema202012(const cld::JsonValue& schema, const std::string& path, std::string& diagnostics)
    {
        if (schema.type == cld::JsonValue::Type::Bool)
        {
            return true;
        }
        if (schema.type != cld::JsonValue::Type::Object)
        {
            diagnostics = path + " must be an object or boolean JSON Schema.";
            return false;
        }

        if (const cld::JsonValue* dialect = JsonMember(schema, "$schema"))
        {
            if (dialect->type != cld::JsonValue::Type::String ||
                (dialect->string != "https://json-schema.org/draft/2020-12/schema" &&
                 dialect->string != "https://json-schema.org/draft/2020-12/schema#"))
            {
                diagnostics = path + ".$schema must select JSON Schema 2020-12.";
                return false;
            }
        }

        if (const cld::JsonValue* type = JsonMember(schema, "type"))
        {
            const auto validTypeName = [](const std::string& name)
            {
                static const std::array<const char*, 7> names = { "null", "boolean", "object", "array", "number", "string", "integer" };
                return std::find_if(names.begin(), names.end(), [&](const char* candidate) { return name == candidate; }) != names.end();
            };
            if (type->type == cld::JsonValue::Type::String)
            {
                if (!validTypeName(type->string))
                {
                    diagnostics = path + ".type contains an unknown JSON Schema type.";
                    return false;
                }
            }
            else if (type->type == cld::JsonValue::Type::Array && !type->array.empty())
            {
                for (const cld::JsonValue& item : type->array)
                {
                    if (item.type != cld::JsonValue::Type::String || !validTypeName(item.string))
                    {
                        diagnostics = path + ".type must contain only JSON Schema type names.";
                        return false;
                    }
                }
            }
            else
            {
                diagnostics = path + ".type must be a string or non-empty string array.";
                return false;
            }
        }

        for (const char* keyword : { "properties", "patternProperties", "$defs", "dependentSchemas" })
        {
            if (const cld::JsonValue* members = JsonMember(schema, keyword))
            {
                if (members->type != cld::JsonValue::Type::Object)
                {
                    diagnostics = path + "." + keyword + " must be an object.";
                    return false;
                }
                for (const auto& [name, child] : members->object)
                {
                    if (!ValidateJsonSchema202012(child, path + "." + keyword + "." + name, diagnostics))
                    {
                        return false;
                    }
                }
            }
        }

        for (const char* keyword : { "items", "contains", "not", "if", "then", "else", "additionalProperties", "unevaluatedProperties", "propertyNames" })
        {
            if (const cld::JsonValue* child = JsonMember(schema, keyword))
            {
                if (!ValidateJsonSchema202012(*child, path + "." + keyword, diagnostics))
                {
                    return false;
                }
            }
        }

        for (const char* keyword : { "allOf", "anyOf", "oneOf", "prefixItems" })
        {
            if (const cld::JsonValue* children = JsonMember(schema, keyword))
            {
                if (children->type != cld::JsonValue::Type::Array || children->array.empty())
                {
                    diagnostics = path + "." + keyword + " must be a non-empty schema array.";
                    return false;
                }
                for (size_t i = 0; i < children->array.size(); ++i)
                {
                    if (!ValidateJsonSchema202012(children->array[i], path + "." + keyword + "[" + std::to_string(i) + "]", diagnostics))
                    {
                        return false;
                    }
                }
            }
        }

        if (const cld::JsonValue* required = JsonMember(schema, "required"))
        {
            if (required->type != cld::JsonValue::Type::Array)
            {
                diagnostics = path + ".required must be an array.";
                return false;
            }
            for (const cld::JsonValue& name : required->array)
            {
                if (name.type != cld::JsonValue::Type::String)
                {
                    diagnostics = path + ".required must contain only strings.";
                    return false;
                }
            }
        }
        if (const cld::JsonValue* values = JsonMember(schema, "enum"); values &&
            (values->type != cld::JsonValue::Type::Array || values->array.empty()))
        {
            diagnostics = path + ".enum must be a non-empty array.";
            return false;
        }
        return true;
    }

    void AppendJoined(std::ostringstream& out, const std::vector<std::string>& items)
    {
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i > 0)
            {
                out << ",";
            }
            out << items[i];
        }
    }

    std::string NewSessionId()
    {
        return mcp::GenerateToken(16);
    }

    bool ConstantTimeEquals(const std::string& left, const std::string& right)
    {
        const size_t count = (std::max)(left.size(), right.size());
        size_t difference = left.size() ^ right.size();
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0;
            const unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0;
            difference |= static_cast<size_t>(a ^ b);
        }
        return difference == 0;
    }

    bool IsLoopbackAuthority(std::string authority)
    {
        authority = ToLowerAscii(cld::TrimAscii(authority));
        if (authority.empty() || authority.find('@') != std::string::npos)
        {
            return false;
        }

        std::string host;
        std::string portText;
        if (authority.front() == '[')
        {
            const size_t closing = authority.find(']');
            if (closing == std::string::npos)
            {
                return false;
            }
            host = authority.substr(1, closing - 1);
            const std::string suffix = authority.substr(closing + 1);
            if (!suffix.empty())
            {
                if (suffix.front() != ':' || suffix.size() == 1) return false;
                portText = suffix.substr(1);
            }
        }
        else
        {
            const size_t colon = authority.find(':');
            if (colon == std::string::npos)
            {
                host = authority;
            }
            else
            {
                if (authority.find(':', colon + 1) != std::string::npos || colon + 1 == authority.size()) return false;
                host = authority.substr(0, colon);
                portText = authority.substr(colon + 1);
            }
        }
        if (host != "127.0.0.1" && host != "localhost" && host != "::1")
        {
            return false;
        }
        if (!portText.empty())
        {
            uint32_t port = 0;
            for (const unsigned char ch : portText)
            {
                if (!std::isdigit(ch)) return false;
                port = port * 10u + static_cast<uint32_t>(ch - '0');
                if (port > 65535u) return false;
            }
            if (port == 0) return false;
        }
        return true;
    }

    bool IsLoopbackOrigin(const std::string& origin)
    {
        const std::string lower = ToLowerAscii(cld::TrimAscii(origin));
        constexpr const char* prefix = "http://";
        if (!StartsWith(lower, prefix))
        {
            return false;
        }
        const std::string authority = lower.substr(std::char_traits<char>::length(prefix));
        return authority.find('/') == std::string::npos && IsLoopbackAuthority(authority);
    }

    int DecodeBase64Character(unsigned char ch)
    {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    }

    bool DecodeHeaderValue(const std::string& encoded, std::string& decoded)
    {
        constexpr const char* sentinel = "=?base64?";
        if (!StartsWith(encoded, sentinel) || encoded.size() < 12 || encoded.compare(encoded.size() - 2, 2, "?=") != 0)
        {
            decoded = encoded;
            return true;
        }
        const std::string payload = encoded.substr(std::char_traits<char>::length(sentinel), encoded.size() - std::char_traits<char>::length(sentinel) - 2);
        if (payload.empty() || payload.size() % 4 != 0)
        {
            return false;
        }
        decoded.clear();
        decoded.reserve(payload.size() / 4 * 3);
        for (size_t i = 0; i < payload.size(); i += 4)
        {
            int values[4]{};
            for (size_t j = 0; j < 4; ++j)
            {
                if (payload[i + j] == '=')
                {
                    values[j] = -2;
                }
                else
                {
                    values[j] = DecodeBase64Character(static_cast<unsigned char>(payload[i + j]));
                    if (values[j] < 0) return false;
                }
            }
            if (values[0] < 0 || values[1] < 0 || (values[2] == -2 && values[3] != -2)) return false;
            const uint32_t bits = (static_cast<uint32_t>(values[0]) << 18) |
                (static_cast<uint32_t>(values[1]) << 12) |
                (static_cast<uint32_t>(values[2] < 0 ? 0 : values[2]) << 6) |
                static_cast<uint32_t>(values[3] < 0 ? 0 : values[3]);
            decoded.push_back(static_cast<char>((bits >> 16) & 0xff));
            if (values[2] >= 0) decoded.push_back(static_cast<char>((bits >> 8) & 0xff));
            if (values[3] >= 0) decoded.push_back(static_cast<char>(bits & 0xff));
            if ((values[2] == -2 || values[3] == -2) && i + 4 != payload.size()) return false;
        }
        return true;
    }

    bool IsSubscribableResourceUri(const std::string& uri)
    {
        static const std::array<const char*, 11> uris =
        {
            "lookdevpt://state",
            "lookdevpt://stats",
            "lookdevpt://diagnostics",
            "lookdevpt://materials",
            "lookdevpt://material-variants",
            "lookdevpt://material-presets",
            "lookdevpt://project",
            "lookdevpt://scene/summary",
            "lookdevpt://captures/index",
            "lookdevpt://captures/latest.png",
            "lookdevpt://actions/schema",
        };
        return std::find_if(uris.begin(), uris.end(), [&](const char* candidate) { return uri == candidate; }) != uris.end();
    }

    int64_t ResourceTtlMs(const std::string& uri)
    {
        if (uri == "lookdevpt://state") return 33;
        if (uri == "lookdevpt://stats" || uri == "lookdevpt://diagnostics") return 100;
        if (uri == "lookdevpt://materials" || StartsWith(uri, "lookdevpt://materials/") || uri == "lookdevpt://material-variants" ||
            uri == "lookdevpt://material-presets" || uri == "lookdevpt://project" || uri == "lookdevpt://scene/summary") return 1000;
        if (uri == "lookdevpt://captures/index" || uri == "lookdevpt://captures/latest.png") return 0;
        if (StartsWith(uri, "lookdevpt://captures/") && uri != "lookdevpt://captures/latest.png") return CatalogTtlMs;
        return CatalogTtlMs;
    }

    std::string SseData(const std::string& json)
    {
        return "data: " + json + "\r\n\r\n";
    }

    std::string PathFromTarget(const std::string& target)
    {
        const size_t query = target.find('?');
        return query == std::string::npos ? target : target.substr(0, query);
    }
}

namespace mcp
{
Server::Server() = default;

Server::~Server()
{
    Stop();
}

bool Server::Start(const ServerSettings& settings, IServerHost* host)
{
    Stop();
    if (!host)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP host was not provided.";
        return false;
    }
    if (settings.authenticationMode == AuthenticationMode::BearerToken &&
        settings.token.empty() && !settings.allowUnauthenticatedLoopbackForTests)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP token is empty.";
        return false;
    }

    try
    {
        const cld::JsonValue catalog = cld::JsonParser(BuildToolsListJson()).Parse();
        const cld::JsonValue* tools = cld::FindMember(catalog, "tools");
        if (!tools || tools->type != cld::JsonValue::Type::Array)
        {
            throw std::runtime_error("MCP tool catalog does not contain a tools array.");
        }
        for (const cld::JsonValue& tool : tools->array)
        {
            const cld::JsonValue* schema = cld::FindMember(tool, "inputSchema");
            std::string schemaDiagnostics;
            if (!schema || schema->type != cld::JsonValue::Type::Object || cld::JsonStringOr(*schema, "type") != "object" ||
                !ValidateJsonSchema202012(*schema, "inputSchema", schemaDiagnostics))
            {
                const std::string toolName = cld::JsonStringOr(tool, "name", "<unnamed>");
                throw std::runtime_error("MCP tool '" + toolName + "' inputSchema is not a valid JSON Schema 2020-12 object: " + schemaDiagnostics);
            }
        }
    }
    catch (const std::exception& ex)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = std::string("MCP tool catalog validation failed: ") + ex.what();
        return false;
    }

    WSADATA wsaData{};
    const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "WSAStartup failed: " + std::to_string(wsaResult);
        return false;
    }

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET)
    {
        const int error = WSAGetLastError();
        WSACleanup();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP socket creation failed: " + std::to_string(error);
        return false;
    }

    BOOL exclusive = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(settings.port);
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(listenSocket);
        WSACleanup();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP bind failed on 127.0.0.1:" + std::to_string(settings.port) + " (" + std::to_string(error) + ")";
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(listenSocket);
        WSACleanup();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "MCP listen failed: " + std::to_string(error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings = settings;
        m_host = host;
        m_listenSocket = static_cast<uintptr_t>(listenSocket);
        m_sessions.clear();
        m_resourceGenerations.clear();
        m_recentRequests.clear();
        m_lastError.clear();
    }
    m_activeConnections = 0;
    m_activeSubscriptions = 0;
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&Server::Run, this);
    AppendLog("MCP server started on 127.0.0.1:" + std::to_string(settings.port));
    return true;
}

void Server::Stop()
{
    if (!m_running && !m_thread.joinable())
    {
        return;
    }

    m_stopRequested = true;
    m_subscriptionCv.notify_all();
    uintptr_t socketValue = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        socketValue = m_listenSocket;
        m_listenSocket = 0;
    }
    if (socketValue != 0 && static_cast<SOCKET>(socketValue) != INVALID_SOCKET)
    {
        shutdown(static_cast<SOCKET>(socketValue), SD_BOTH);
        closesocket(static_cast<SOCKET>(socketValue));
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (Worker& worker : m_workers)
        {
            if (worker.socketValue != 0 &&
                (!worker.complete || !worker.complete->load()) &&
                (!worker.subscriptionStream ||
                    !worker.subscriptionStream->load()) &&
                (!worker.socketClosed ||
                    !worker.socketClosed->exchange(true)))
            {
                const SOCKET workerSocket =
                    static_cast<SOCKET>(worker.socketValue);
                shutdown(workerSocket, SD_BOTH);
                closesocket(workerSocket);
            }
        }
    }

    if (m_thread.joinable())
    {
        m_thread.join();
    }

    std::vector<Worker> workers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        workers.swap(m_workers);
        m_sessions.clear();
    }
    for (Worker& worker : workers)
    {
        if (worker.thread.joinable())
        {
            worker.thread.join();
        }
    }

    m_running = false;
    WSACleanup();
}

bool Server::IsRunning() const
{
    return m_running;
}

ServerStatus Server::GetStatus() const
{
    ServerStatus status;
    std::lock_guard<std::mutex> lock(m_mutex);
    status.running = m_running;
    status.port = m_settings.port;
    status.endpoint = "http://127.0.0.1:" + std::to_string(m_settings.port) + "/mcp";
    status.lastError = m_lastError;
    status.activeLegacySessions = m_sessions.size();
    status.activeSubscriptions = m_activeSubscriptions.load();
    status.activeRequests = m_activeRequests.load();
    status.recentRequests = m_recentRequests;
    return status;
}

void Server::PublishResourceUpdates(const std::vector<std::string>& uris)
{
    if (!m_running || uris.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::string& uri : uris)
        {
            if (!uri.empty())
            {
                ++m_resourceGenerations[uri];
            }
        }
    }
    m_subscriptionCv.notify_all();
}

void Server::Run()
{
    while (!m_stopRequested)
    {
        ReapWorkers();
        SOCKET listenSocket = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            PruneLegacySessionsLocked(std::chrono::steady_clock::now());
            listenSocket = static_cast<SOCKET>(m_listenSocket);
        }
        if (listenSocket == INVALID_SOCKET || listenSocket == 0)
        {
            break;
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenSocket, &readable);
        timeval timeout{};
        timeout.tv_sec = 1;
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready == 0)
        {
            continue;
        }
        if (ready == SOCKET_ERROR)
        {
            if (!m_stopRequested)
            {
                const int error = WSAGetLastError();
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastError = "MCP socket wait failed: " + std::to_string(error);
            }
            break;
        }

        SOCKET client = accept(listenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            if (!m_stopRequested)
            {
                const int error = WSAGetLastError();
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lastError = "MCP accept failed: " + std::to_string(error);
            }
            break;
        }
        if (m_stopRequested)
        {
            shutdown(client, SD_BOTH);
            closesocket(client);
            break;
        }

        if (m_activeConnections.load() >= MaxConnections)
        {
            const HttpResponse response = JsonResponse(503, "Service Unavailable", MakeHttpErrorBody(-32000, "MCP connection limit reached."));
            SendHttpResponse(static_cast<uintptr_t>(client), response);
            shutdown(client, SD_BOTH);
            closesocket(client);
            continue;
        }

        const auto complete = std::make_shared<std::atomic<bool>>(false);
        const auto socketClosed =
            std::make_shared<std::atomic<bool>>(false);
        const auto subscriptionStream =
            std::make_shared<std::atomic<bool>>(false);
        m_activeConnections.fetch_add(1);
        bool workerStarted = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_stopRequested)
            {
                Worker worker;
                worker.complete = complete;
                worker.socketClosed = socketClosed;
                worker.subscriptionStream = subscriptionStream;
                worker.socketValue = static_cast<uintptr_t>(client);
                worker.thread = std::thread(
                    &Server::HandleClient,
                    this,
                    static_cast<uintptr_t>(client),
                    complete,
                    socketClosed,
                    subscriptionStream);
                m_workers.push_back(std::move(worker));
                workerStarted = true;
            }
        }
        if (!workerStarted)
        {
            m_activeConnections.fetch_sub(1);
            shutdown(client, SD_BOTH);
            closesocket(client);
            break;
        }
    }
    ReapWorkers();
}

void Server::ReapWorkers()
{
    std::vector<std::thread> finished;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_workers.begin();
        while (it != m_workers.end())
        {
            if (it->complete && it->complete->load())
            {
                finished.push_back(std::move(it->thread));
                it = m_workers.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for (std::thread& worker : finished)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

void Server::HandleClient(
    uintptr_t socketValue,
    const std::shared_ptr<std::atomic<bool>>& complete,
    const std::shared_ptr<std::atomic<bool>>& socketClosed,
    const std::shared_ptr<std::atomic<bool>>& subscriptionStream)
{
    SOCKET client = static_cast<SOCKET>(socketValue);
    auto connectionGuard = std::unique_ptr<void, std::function<void(void*)>>(reinterpret_cast<void*>(1), [this, &complete](void*)
    {
        m_activeConnections.fetch_sub(1);
        complete->store(true);
    });
    DWORD timeoutMs = 30000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    HttpRequest request;
    int errorStatus = 400;
    std::string error;
    HttpResponse response;
    if (!ReadHttpRequest(socketValue, request, errorStatus, error))
    {
        const int errorCode = errorStatus == 400 ? -32700 : -32000;
        response = JsonResponse(
            errorStatus,
            HttpReasonPhrase(errorStatus),
            MakeHttpErrorBody(errorCode, error));
    }
    else
    {
        response = HandleHttpRequest(request);
    }

    if (response.subscriptionStream)
    {
        if (m_activeSubscriptions.fetch_add(1) >= MaxSubscriptions)
        {
            m_activeSubscriptions.fetch_sub(1);
            response = JsonResponse(429, "Too Many Requests", MakeError(response.subscriptionIdJson, -32000, "MCP subscription limit reached."));
            SendHttpResponse(socketValue, response);
        }
        else
        {
            auto subscriptionGuard = std::unique_ptr<void, std::function<void(void*)>>(reinterpret_cast<void*>(1), [this](void*)
            {
                m_activeSubscriptions.fetch_sub(1);
            });
            subscriptionStream->store(true);
            StreamSubscription(socketValue, response);
            subscriptionStream->store(false);
        }
    }
    else
    {
        SendHttpResponse(socketValue, response);
    }
    if (!socketClosed->exchange(true))
    {
        shutdown(client, SD_BOTH);
        closesocket(client);
    }
}

bool Server::ReadHttpRequest(
    uintptr_t socketValue,
    HttpRequest& request,
    int& errorStatus,
    std::string& error) const
{
    errorStatus = 400;
    SOCKET client = static_cast<SOCKET>(socketValue);
    std::string buffer;
    char chunk[4096];
    size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos)
    {
        const int received = recv(client, chunk, sizeof(chunk), 0);
        if (received <= 0)
        {
            error = "HTTP request read failed.";
            return false;
        }
        buffer.append(chunk, chunk + received);
        if (buffer.size() > MaxHttpHeaderBytes)
        {
            errorStatus = 431;
            error = "HTTP headers are too large.";
            return false;
        }
        headerEnd = buffer.find("\r\n\r\n");
    }

    const std::string headerText = buffer.substr(0, headerEnd);
    std::istringstream headerStream(headerText);
    std::string requestLine;
    if (!std::getline(headerStream, requestLine))
    {
        error = "HTTP request line missing.";
        return false;
    }
    if (!requestLine.empty() && requestLine.back() == '\r')
    {
        requestLine.pop_back();
    }

    std::istringstream requestLineStream(requestLine);
    std::string version;
    requestLineStream >> request.method >> request.target >> version;
    if (request.method.empty() || request.target.empty() || version != "HTTP/1.1")
    {
        error = "HTTP request line is invalid.";
        return false;
    }
    request.path = PathFromTarget(request.target);

    std::string line;
    while (std::getline(headerStream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        std::string name = ToLowerAscii(cld::TrimAscii(line.substr(0, colon)));
        std::string value = cld::TrimAscii(line.substr(colon + 1));
        request.headers[name] = value;
    }

    size_t contentLength = 0;
    const std::string contentLengthText = HeaderValue(request.headers, "content-length");
    const std::string transferEncoding = ToLowerAscii(HeaderValue(request.headers, "transfer-encoding"));
    if (!contentLengthText.empty() && !transferEncoding.empty())
    {
        error = "Content-Length and Transfer-Encoding cannot be combined.";
        return false;
    }
    if (!contentLengthText.empty())
    {
        if (!std::all_of(contentLengthText.begin(), contentLengthText.end(), [](unsigned char value)
        {
            return std::isdigit(value) != 0;
        }))
        {
            error = "Content-Length is invalid.";
            return false;
        }
        try
        {
            contentLength = static_cast<size_t>(std::stoull(contentLengthText));
        }
        catch (...)
        {
            error = "Content-Length is invalid.";
            return false;
        }
        if (contentLength > MaxHttpBodyBytes)
        {
            errorStatus = 413;
            error = "HTTP body is too large.";
            return false;
        }
    }

    const size_t bodyStart = headerEnd + 4;
    if (!transferEncoding.empty())
    {
        const std::string normalizedTransferEncoding =
            cld::TrimAscii(transferEncoding);
        if (normalizedTransferEncoding != "chunked")
        {
            const size_t finalComma = normalizedTransferEncoding.rfind(',');
            const bool finalCodingIsChunked = finalComma != std::string::npos &&
                cld::TrimAscii(normalizedTransferEncoding.substr(finalComma + 1)) ==
                    "chunked";
            if (finalCodingIsChunked)
            {
                const std::string preceding =
                    cld::TrimAscii(normalizedTransferEncoding.substr(0, finalComma));
                if (preceding.find("chunked") != std::string::npos)
                {
                    error = "The chunked transfer coding must appear exactly once and last.";
                    return false;
                }
                errorStatus = 501;
                error = "Transfer coding is not supported.";
                return false;
            }
            error = "The final transfer coding must be chunked.";
            return false;
        }

        std::string encoded = buffer.substr(bodyStart);
        size_t cursor = 0;
        size_t chunkCount = 0;
        request.body.clear();
        auto receiveMore = [&]() -> bool
        {
            const int received = recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0)
            {
                error = "Chunked HTTP body read failed.";
                return false;
            }
            encoded.append(chunk, chunk + received);
            return true;
        };
        auto ensureBytes = [&](size_t required) -> bool
        {
            while (encoded.size() < required)
            {
                if (!receiveMore()) return false;
            }
            return true;
        };

        for (;;)
        {
            if (++chunkCount > MaxHttpChunks)
            {
                error = "Chunked HTTP body has too many chunks.";
                return false;
            }

            size_t lineEnd = encoded.find("\r\n", cursor);
            while (lineEnd == std::string::npos)
            {
                if (encoded.size() - cursor > MaxHttpHeaderBytes)
                {
                    error = "HTTP chunk header is too large.";
                    return false;
                }
                if (!receiveMore()) return false;
                lineEnd = encoded.find("\r\n", cursor);
            }

            std::string sizeText = encoded.substr(cursor, lineEnd - cursor);
            const size_t extension = sizeText.find(';');
            if (extension != std::string::npos) sizeText.resize(extension);
            sizeText = cld::TrimAscii(sizeText);
            if (sizeText.empty() || !std::all_of(sizeText.begin(), sizeText.end(), [](unsigned char value)
            {
                return std::isxdigit(value) != 0;
            }))
            {
                error = "HTTP chunk size is invalid.";
                return false;
            }

            uint64_t chunkSize = 0;
            try
            {
                chunkSize = std::stoull(sizeText, nullptr, 16);
            }
            catch (...)
            {
                error = "HTTP chunk size is invalid.";
                return false;
            }
            cursor = lineEnd + 2;

            if (chunkSize == 0)
            {
                if (!ensureBytes(cursor + 2)) return false;
                if (encoded.compare(cursor, 2, "\r\n") == 0) return true;

                size_t trailerEnd = encoded.find("\r\n\r\n", cursor);
                while (trailerEnd == std::string::npos)
                {
                    if (encoded.size() - cursor > MaxHttpHeaderBytes)
                    {
                        error = "HTTP chunk trailers are too large.";
                        return false;
                    }
                    if (!receiveMore()) return false;
                    trailerEnd = encoded.find("\r\n\r\n", cursor);
                }
                return true;
            }

            if (chunkSize > MaxHttpBodyBytes - request.body.size())
            {
                errorStatus = 413;
                error = "HTTP body is too large.";
                return false;
            }
            const size_t nativeChunkSize = static_cast<size_t>(chunkSize);
            if (!ensureBytes(cursor + nativeChunkSize + 2)) return false;
            request.body.append(encoded, cursor, nativeChunkSize);
            cursor += nativeChunkSize;
            if (encoded.compare(cursor, 2, "\r\n") != 0)
            {
                error = "HTTP chunk terminator is invalid.";
                return false;
            }
            cursor += 2;

            if (cursor >= MaxHttpHeaderBytes)
            {
                encoded.erase(0, cursor);
                cursor = 0;
            }
        }
    }

    request.body = buffer.substr(bodyStart);
    while (request.body.size() < contentLength)
    {
        const int received = recv(client, chunk, sizeof(chunk), 0);
        if (received <= 0)
        {
            error = "HTTP body read failed.";
            return false;
        }
        request.body.append(chunk, chunk + received);
    }
    if (request.body.size() > contentLength)
    {
        request.body.resize(contentLength);
    }
    return true;
}

bool Server::SendBytes(uintptr_t socketValue, const std::string& bytes) const
{
    SOCKET client = static_cast<SOCKET>(socketValue);
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const size_t remaining = bytes.size() - offset;
        const int chunkSize = static_cast<int>((std::min)(remaining, static_cast<size_t>(INT_MAX)));
        const int sent = send(client, bytes.data() + offset, chunkSize, 0);
        if (sent <= 0)
        {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool Server::SendHttpResponse(uintptr_t socketValue, const HttpResponse& response) const
{
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " " << response.reason << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    if (!response.contentType.empty())
    {
        out << "Content-Type: " << response.contentType << "\r\n";
    }
    out << "Connection: close\r\n";
    for (const auto& [name, value] : response.headers)
    {
        out << name << ": " << value << "\r\n";
    }
    out << "\r\n";
    out << response.body;
    return SendBytes(socketValue, out.str());
}

void Server::StreamSubscription(uintptr_t socketValue, const HttpResponse& response)
{
    const auto keepAlive = m_settings.subscriptionKeepAliveMillisecondsForTests > 0
        ? std::chrono::milliseconds(m_settings.subscriptionKeepAliveMillisecondsForTests)
        : std::chrono::duration_cast<std::chrono::milliseconds>(SubscriptionKeepAlive);
    std::map<std::string, uint64_t> observed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const std::string& uri : response.subscriptionUris)
        {
            observed[uri] = m_resourceGenerations[uri];
        }
    }

    std::ostringstream headers;
    headers << "HTTP/1.1 200 OK\r\n";
    headers << "Content-Type: text/event-stream\r\n";
    headers << "Cache-Control: no-cache\r\n";
    headers << "Connection: keep-alive\r\n";
    headers << "X-Accel-Buffering: no\r\n\r\n";
    if (!SendBytes(socketValue, headers.str()))
    {
        return;
    }

    std::ostringstream acknowledged;
    acknowledged << "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/subscriptions/acknowledged\",\"params\":{";
    acknowledged << "\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":" << response.subscriptionIdJson << "},";
    acknowledged << "\"notifications\":{\"resourceSubscriptions\":[";
    for (size_t i = 0; i < response.subscriptionUris.size(); ++i)
    {
        if (i > 0) acknowledged << ",";
        acknowledged << "\"" << cld::EscapeJson(response.subscriptionUris[i]) << "\"";
    }
    acknowledged << "]}}}";
    if (!SendBytes(socketValue, SseData(acknowledged.str())))
    {
        return;
    }

    while (!m_stopRequested)
    {
        std::vector<std::string> changed;
        bool timedOut = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            const bool signalled = m_subscriptionCv.wait_for(lock, keepAlive, [&]()
            {
                if (m_stopRequested) return true;
                for (const auto& [uri, generation] : observed)
                {
                    const auto it = m_resourceGenerations.find(uri);
                    const uint64_t current = it == m_resourceGenerations.end() ? 0 : it->second;
                    if (current != generation) return true;
                }
                return false;
            });
            timedOut = !signalled;
            if (!m_stopRequested)
            {
                for (auto& [uri, generation] : observed)
                {
                    const auto it = m_resourceGenerations.find(uri);
                    const uint64_t current = it == m_resourceGenerations.end() ? 0 : it->second;
                    if (current != generation)
                    {
                        generation = current;
                        changed.push_back(uri);
                    }
                }
            }
        }

        if (m_stopRequested)
        {
            const std::string finalResult = "{\"jsonrpc\":\"2.0\",\"id\":" + response.subscriptionIdJson +
                ",\"result\":{\"resultType\":\"complete\",\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":" +
                response.subscriptionIdJson + ",\"io.modelcontextprotocol/serverInfo\":" + ServerInfoJson() + "}}}";
            SendBytes(socketValue, SseData(finalResult));
            break;
        }

        if (timedOut && changed.empty())
        {
            if (!SendBytes(socketValue, ":\r\n\r\n"))
            {
                break;
            }
            continue;
        }

        bool connected = true;
        for (const std::string& uri : changed)
        {
            const std::string notification = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/updated\",\"params\":{"
                "\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":" + response.subscriptionIdJson + "},"
                "\"uri\":\"" + cld::EscapeJson(uri) + "\"}}";
            if (!SendBytes(socketValue, SseData(notification)))
            {
                connected = false;
                break;
            }
        }
        if (!connected)
        {
            break;
        }
    }
}

Server::HttpResponse Server::HandleHttpRequest(const HttpRequest& request)
{
    if (request.path != "/mcp")
    {
        return JsonResponse(404, "Not Found", MakeHttpErrorBody(-32601, "MCP endpoint is /mcp."));
    }
    if (!ValidateOrigin(request))
    {
        return JsonResponse(403, "Forbidden", MakeHttpErrorBody(-32000, "Origin is not allowed."));
    }
    if (!ValidateHost(request))
    {
        return JsonResponse(403, "Forbidden", MakeHttpErrorBody(-32000, "Host is not allowed."));
    }
    if (!ValidateAuthorization(request))
    {
        return JsonResponse(401, "Unauthorized", MakeHttpErrorBody(-32001, "Authorization bearer token is required."));
    }
    const std::string protocolVersion = HeaderValue(request.headers, "mcp-protocol-version");
    bool modern = protocolVersion == ModernProtocolVersion;
    if (request.method == "GET")
    {
        HttpResponse response = JsonResponse(405, "Method Not Allowed", MakeHttpErrorBody(-32000, "MCP GET streams are not supported."));
        response.headers.push_back({ "Allow", "POST, DELETE" });
        return response;
    }
    if (request.method == "DELETE")
    {
        if (modern)
        {
            HttpResponse response = JsonResponse(405, "Method Not Allowed", MakeHttpErrorBody(-32000, "Protocol 2026-07-28 does not use sessions."));
            response.headers.push_back({ "Allow", "POST" });
            return response;
        }
        return HandleDeleteSession(request);
    }
    if (request.method != "POST")
    {
        HttpResponse response = JsonResponse(405, "Method Not Allowed", MakeHttpErrorBody(-32000, "Only POST, GET, and DELETE are supported."));
        response.headers.push_back({ "Allow", "POST, GET, DELETE" });
        return response;
    }

    const std::string accept = HeaderValue(request.headers, "accept");
    if (modern)
    {
        const std::string lowerAccept = ToLowerAscii(accept);
        if (lowerAccept.find("application/json") == std::string::npos || lowerAccept.find("text/event-stream") == std::string::npos)
        {
            return JsonResponse(406, "Not Acceptable", MakeHttpErrorBody(-32000, "Accept header must list application/json and text/event-stream."));
        }
        const std::string contentType = ToLowerAscii(HeaderValue(request.headers, "content-type"));
        if (contentType.find("application/json") == std::string::npos)
        {
            return JsonResponse(415, "Unsupported Media Type", MakeHttpErrorBody(-32600, "Content-Type must be application/json."));
        }
    }
    else if (!accept.empty() && !ContainsHeaderToken(accept, "application/json") && !ContainsHeaderToken(accept, "text/event-stream"))
    {
        return JsonResponse(406, "Not Acceptable", MakeHttpErrorBody(-32000, "Accept header must allow application/json or text/event-stream."));
    }

    cld::JsonValue rpc;
    try
    {
        rpc = cld::JsonParser(request.body).Parse();
    }
    catch (const std::exception& ex)
    {
        return JsonResponse(400, "Bad Request", MakeHttpErrorBody(-32700, ex.what()));
    }
    const std::string idJson = rpc.type == cld::JsonValue::Type::Object ? JsonIdToJson(rpc) : "null";
    if (!protocolVersion.empty() && !IsSupportedProtocolVersion(protocolVersion))
    {
        const std::string data = "{\"supported\":" + SupportedVersionsJson() + ",\"requested\":\"" + cld::EscapeJson(protocolVersion) + "\"}";
        return JsonResponse(400, "Bad Request", MakeErrorWithData(idJson, -32022, "Unsupported protocol version", data));
    }
    if (!modern && rpc.type == cld::JsonValue::Type::Object)
    {
        const cld::JsonValue* params = JsonMember(rpc, "params");
        const cld::JsonValue* meta = params && params->type == cld::JsonValue::Type::Object ? JsonMember(*params, "_meta") : nullptr;
        const cld::JsonValue* bodyVersion = meta && meta->type == cld::JsonValue::Type::Object ? JsonMember(*meta, "io.modelcontextprotocol/protocolVersion") : nullptr;
        if (bodyVersion && bodyVersion->type == cld::JsonValue::Type::String && !IsSupportedProtocolVersion(bodyVersion->string))
        {
            const std::string data = "{\"supported\":" + SupportedVersionsJson() + ",\"requested\":\"" + cld::EscapeJson(bodyVersion->string) + "\"}";
            return JsonResponse(400, "Bad Request", MakeErrorWithData(idJson, -32022, "Unsupported protocol version", data));
        }
        modern = bodyVersion && bodyVersion->type == cld::JsonValue::Type::String && bodyVersion->string == ModernProtocolVersion;
        if (modern)
        {
            const std::string lowerAccept = ToLowerAscii(accept);
            const std::string contentType = ToLowerAscii(HeaderValue(request.headers, "content-type"));
            if (lowerAccept.find("application/json") == std::string::npos || lowerAccept.find("text/event-stream") == std::string::npos)
            {
                return JsonResponse(406, "Not Acceptable", MakeHttpErrorBody(-32000, "Accept header must list application/json and text/event-stream."));
            }
            if (contentType.find("application/json") == std::string::npos)
            {
                return JsonResponse(415, "Unsupported Media Type", MakeHttpErrorBody(-32600, "Content-Type must be application/json."));
            }
        }
    }
    if (modern)
    {
        int errorCode = -32602;
        std::string diagnostics;
        if (!ValidateModernRequest(request, rpc, errorCode, diagnostics))
        {
            return JsonResponse(400, "Bad Request", MakeError(idJson, errorCode, diagnostics));
        }
    }
    else
    {
        std::string protocolDiagnostics;
        if (!ValidateProtocolHeader(request, protocolDiagnostics))
        {
            return JsonResponse(400, "Bad Request", MakeHttpErrorBody(-32000, protocolDiagnostics));
        }
    }
    return HandleJsonRpc(request, rpc, modern);
}

Server::HttpResponse Server::HandleJsonRpc(const HttpRequest& request, const cld::JsonValue& rpc, bool modern)
{
    if (rpc.type != cld::JsonValue::Type::Object)
    {
        return JsonResponse(400, "Bad Request", MakeError("null", -32600, "JSON-RPC message must be an object."));
    }

    if (JsonMemberString(rpc, "jsonrpc") != "2.0")
    {
        return JsonResponse(400, "Bad Request", MakeError(JsonIdToJson(rpc), -32600, "jsonrpc must be 2.0."));
    }

    const std::string method = JsonMemberString(rpc, "method");
    const bool hasId = HasJsonId(rpc);
    if (method.empty())
    {
        return JsonResponse(202, "Accepted", "");
    }

    if (!modern && method == "initialize")
    {
        if (!hasId)
        {
            return JsonResponse(202, "Accepted", "");
        }
        return HandleInitialize(rpc);
    }

    if (!modern)
    {
        Session session;
        std::string sessionDiagnostics;
        if (!ResolveSession(request, session, sessionDiagnostics))
        {
            return JsonResponse(sessionDiagnostics == "Unknown MCP session." ? 404 : 400, sessionDiagnostics == "Unknown MCP session." ? "Not Found" : "Bad Request", MakeError(JsonIdToJson(rpc), -32002, sessionDiagnostics));
        }
    }

    if (!hasId)
    {
        if (!modern && method == "notifications/initialized")
        {
            MarkSessionInitialized(request);
            AppendLog("notifications/initialized");
        }
        return JsonResponse(202, "Accepted", "");
    }

    m_activeRequests.fetch_add(1);
    auto requestGuard = std::unique_ptr<void, std::function<void(void*)>>(reinterpret_cast<void*>(1), [this](void*)
    {
        m_activeRequests.fetch_sub(1);
    });

    const std::string idJson = JsonIdToJson(rpc);
    AppendLog(method);

    if (modern && method == "server/discover")
    {
        const std::string result = "{\"resultType\":\"complete\",\"supportedVersions\":" + SupportedVersionsJson() +
            ",\"capabilities\":{\"tools\":{\"listChanged\":false},\"resources\":{\"subscribe\":true,\"listChanged\":false},\"prompts\":{\"listChanged\":false}},"
            "\"instructions\":\"Use lookdevpt.* tools to inspect and control the local D3D12LookDevPT session.\","
            "\"ttlMs\":" + std::to_string(CatalogTtlMs) + ",\"cacheScope\":\"private\",\"_meta\":{\"io.modelcontextprotocol/serverInfo\":" + ServerInfoJson() + "}}";
        return JsonResponse(200, "OK", MakeResponse(idJson, result));
    }
    if (!modern && method == "ping")
    {
        return JsonResponse(200, "OK", MakeResponse(idJson, "{}"));
    }
    if (modern && method == "subscriptions/listen")
    {
        const cld::JsonValue* params = JsonMember(rpc, "params");
        const cld::JsonValue* notifications = params ? JsonMember(*params, "notifications") : nullptr;
        if (!params || params->type != cld::JsonValue::Type::Object || !notifications || notifications->type != cld::JsonValue::Type::Object)
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "subscriptions/listen requires params.notifications."));
        }
        std::vector<std::string> uris;
        if (const cld::JsonValue* requestedUris = JsonMember(*notifications, "resourceSubscriptions"))
        {
            if (requestedUris->type != cld::JsonValue::Type::Array || requestedUris->array.size() > MaxSubscriptionUris)
            {
                return JsonResponse(200, "OK", MakeError(idJson, -32602, "resourceSubscriptions must be an array of at most 64 URIs."));
            }
            for (const cld::JsonValue& value : requestedUris->array)
            {
                if (value.type != cld::JsonValue::Type::String)
                {
                    return JsonResponse(200, "OK", MakeError(idJson, -32602, "resourceSubscriptions entries must be strings."));
                }
                if (IsSubscribableResourceUri(value.string) && std::find(uris.begin(), uris.end(), value.string) == uris.end())
                {
                    uris.push_back(value.string);
                }
            }
        }
        HttpResponse response;
        response.subscriptionStream = true;
        response.subscriptionIdJson = idJson;
        response.subscriptionUris = std::move(uris);
        return response;
    }
    auto rejectCursor = [&]() -> bool
    {
        if (!modern) return false;
        const cld::JsonValue* params = JsonMember(rpc, "params");
        return params && params->type == cld::JsonValue::Type::Object && JsonMember(*params, "cursor") != nullptr;
    };
    if (method == "tools/list")
    {
        if (rejectCursor()) return JsonResponse(200, "OK", MakeError(idJson, -32602, "This server did not issue the supplied cursor."));
        return JsonResponse(200, "OK", ProtocolResponse(idJson, BuildToolsListJson(), modern, CatalogTtlMs));
    }
    if (method == "resources/list")
    {
        if (rejectCursor()) return JsonResponse(200, "OK", MakeError(idJson, -32602, "This server did not issue the supplied cursor."));
        return JsonResponse(200, "OK", ProtocolResponse(idJson, BuildResourcesListJson(), modern, CatalogTtlMs));
    }
    if (method == "resources/templates/list")
    {
        if (rejectCursor()) return JsonResponse(200, "OK", MakeError(idJson, -32602, "This server did not issue the supplied cursor."));
        return JsonResponse(200, "OK", ProtocolResponse(idJson, BuildResourceTemplatesListJson(), modern, CatalogTtlMs));
    }
    if (method == "prompts/list")
    {
        if (rejectCursor()) return JsonResponse(200, "OK", MakeError(idJson, -32602, "This server did not issue the supplied cursor."));
        return JsonResponse(200, "OK", ProtocolResponse(idJson, BuildPromptsListJson(), modern, CatalogTtlMs));
    }
    if (method == "prompts/get")
    {
        return HandlePromptGet(idJson, rpc, modern);
    }
    if (method == "resources/read")
    {
        const cld::JsonValue* params = JsonMember(rpc, "params");
        if (!params || params->type != cld::JsonValue::Type::Object)
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "resources/read requires params."));
        }
        const std::string uri = cld::JsonStringOr(*params, "uri");
        if (uri.empty())
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "resources/read requires uri."));
        }
        ResourceResult resource = m_host->ReadMcpResource(uri);
        if (!resource.ok)
        {
            const int errorCode = modern ? -32602 : -32004;
            const std::string message = resource.error.empty() ? "Resource read failed." : resource.error;
            if (modern)
            {
                return JsonResponse(200, "OK", MakeErrorWithData(idJson, errorCode, message,
                    "{\"uri\":\"" + cld::EscapeJson(uri) + "\"}"));
            }
            return JsonResponse(200, "OK", MakeError(idJson, errorCode, message));
        }
        std::string content = "{\"uri\":\"" + cld::EscapeJson(resource.uri.empty() ? uri : resource.uri) + "\",\"mimeType\":\"" + cld::EscapeJson(resource.mimeType) + "\"";
        if (!resource.blob.empty())
        {
            content += ",\"blob\":\"" + resource.blob + "\"";
        }
        else
        {
            content += ",\"text\":\"" + cld::EscapeJson(resource.text) + "\"";
        }
        content += "}";
        return JsonResponse(200, "OK", ProtocolResponse(idJson, "{\"contents\":[" + content + "]}", modern, ResourceTtlMs(uri)));
    }
    if (method == "tools/call")
    {
        const cld::JsonValue* params = JsonMember(rpc, "params");
        if (!params || params->type != cld::JsonValue::Type::Object)
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "tools/call requires params."));
        }
        const std::string name = cld::JsonStringOr(*params, "name");
        if (name.empty())
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "tools/call requires name."));
        }
        const cld::JsonValue* arguments = JsonMember(*params, "arguments");
        cld::JsonValue emptyArguments;
        emptyArguments.type = cld::JsonValue::Type::Object;
        if (!arguments)
        {
            arguments = &emptyArguments;
        }
        if (arguments->type != cld::JsonValue::Type::Object)
        {
            return JsonResponse(200, "OK", MakeError(idJson, -32602, "tools/call arguments must be an object."));
        }

        ToolResult toolResult = m_host->CallMcpTool(name, *arguments, (std::max)(1, m_settings.requestTimeoutSeconds) * 1000);
        return JsonResponse(200, "OK", ProtocolResponse(idJson, ToolResultToJson(toolResult), modern));
    }

    return JsonResponse(modern ? 404 : 200, modern ? "Not Found" : "OK", MakeError(idJson, -32601, "Unsupported MCP method."));
}

Server::HttpResponse Server::HandleInitialize(const cld::JsonValue& rpc)
{
    std::string requestedVersion = LegacyProtocolVersion;
    if (const cld::JsonValue* params = JsonMember(rpc, "params"); params && params->type == cld::JsonValue::Type::Object)
    {
        requestedVersion = cld::JsonStringOr(*params, "protocolVersion", LegacyProtocolVersion);
    }
    const std::string negotiatedVersion = IsLegacyProtocolVersion(requestedVersion) ? requestedVersion : LegacyProtocolVersion;
    const std::string sessionId = NewSessionId();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        PruneLegacySessionsLocked(now);
        if (m_sessions.size() >= MaxLegacySessions)
        {
            return JsonResponse(429, "Too Many Requests", MakeError(JsonIdToJson(rpc), -32000, "MCP legacy session limit reached."));
        }
        m_sessions[sessionId] = Session{ negotiatedVersion, false, now };
    }

    std::string result = "{\"protocolVersion\":\"" + negotiatedVersion + "\","
        "\"capabilities\":{\"tools\":{\"listChanged\":false},\"resources\":{\"listChanged\":false},\"prompts\":{\"listChanged\":false}},"
        "\"serverInfo\":" + std::string(ServerInfoJson()) + ","
        "\"instructions\":\"Use lookdevpt.* tools to inspect and control the local D3D12LookDevPT session.\"}";
    HttpResponse response = JsonResponse(200, "OK", MakeResponse(JsonIdToJson(rpc), result));
    response.headers.push_back({ "MCP-Session-Id", sessionId });
    AppendLog("initialize");
    return response;
}

Server::HttpResponse Server::HandlePromptGet(const std::string& idJson, const cld::JsonValue& rpc, bool modern)
{
    const cld::JsonValue* params = JsonMember(rpc, "params");
    if (!params || params->type != cld::JsonValue::Type::Object)
    {
        return JsonResponse(200, "OK", MakeError(idJson, -32602, "prompts/get requires params."));
    }
    const std::string name = cld::JsonStringOr(*params, "name");
    if (name.empty())
    {
        return JsonResponse(200, "OK", MakeError(idJson, -32602, "prompts/get requires name."));
    }
    const cld::JsonValue* arguments = JsonMember(*params, "arguments");
    bool found = false;
    const std::string result = BuildPromptGetResultJson(name, arguments, found);
    if (!found)
    {
        return JsonResponse(200, "OK", MakeError(idJson, modern ? -32602 : -32004, "Unknown prompt."));
    }
    return JsonResponse(200, "OK", ProtocolResponse(idJson, result, modern));
}

Server::HttpResponse Server::HandleDeleteSession(const HttpRequest& request)
{
    const std::string sessionId = HeaderValue(request.headers, "mcp-session-id");
    if (sessionId.empty())
    {
        return JsonResponse(400, "Bad Request", MakeHttpErrorBody(-32002, "MCP-Session-Id is required."));
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end())
        {
            return JsonResponse(404, "Not Found", MakeHttpErrorBody(-32002, "Unknown MCP session."));
        }
        m_sessions.erase(it);
    }
    AppendLog("DELETE session");
    return JsonResponse(202, "Accepted", "");
}

bool Server::ValidateOrigin(const HttpRequest& request) const
{
    const std::string origin = HeaderValue(request.headers, "origin");
    if (origin.empty())
    {
        return true;
    }
    return IsLoopbackOrigin(origin);
}

bool Server::ValidateHost(const HttpRequest& request) const
{
    return IsLoopbackAuthority(HeaderValue(request.headers, "host"));
}

bool Server::ValidateAuthorization(const HttpRequest& request) const
{
    if (m_settings.authenticationMode == AuthenticationMode::None ||
        m_settings.allowUnauthenticatedLoopbackForTests)
    {
        return true;
    }
    const std::string authorization = HeaderValue(request.headers, "authorization");
    return ConstantTimeEquals(authorization, "Bearer " + m_settings.token);
}

bool Server::ValidateProtocolHeader(const HttpRequest& request, std::string& diagnostics) const
{
    const std::string version = HeaderValue(request.headers, "mcp-protocol-version");
    if (version.empty())
    {
        return true;
    }
    if (!IsLegacyProtocolVersion(version))
    {
        diagnostics = "Unsupported MCP-Protocol-Version.";
        return false;
    }
    return true;
}

bool Server::ValidateModernRequest(const HttpRequest& request, const cld::JsonValue& rpc, int& errorCode, std::string& diagnostics) const
{
    if (rpc.type != cld::JsonValue::Type::Object)
    {
        errorCode = -32600;
        diagnostics = "JSON-RPC message must be an object.";
        return false;
    }
    const std::string method = JsonMemberString(rpc, "method");
    const std::string methodHeader = HeaderValue(request.headers, "mcp-method");
    if (methodHeader.empty() || methodHeader != method)
    {
        errorCode = -32020;
        diagnostics = "Mcp-Method header is missing or does not match the request method.";
        return false;
    }
    const cld::JsonValue* params = JsonMember(rpc, "params");
    const cld::JsonValue* meta = params && params->type == cld::JsonValue::Type::Object ? JsonMember(*params, "_meta") : nullptr;
    if (!meta || meta->type != cld::JsonValue::Type::Object)
    {
        errorCode = -32602;
        diagnostics = "Every 2026-07-28 request requires params._meta.";
        return false;
    }
    const cld::JsonValue* protocolValue = JsonMember(*meta, "io.modelcontextprotocol/protocolVersion");
    const cld::JsonValue* capabilities = JsonMember(*meta, "io.modelcontextprotocol/clientCapabilities");
    if (!protocolValue || protocolValue->type != cld::JsonValue::Type::String || !capabilities || capabilities->type != cld::JsonValue::Type::Object)
    {
        errorCode = -32602;
        diagnostics = "Required protocolVersion or clientCapabilities metadata is missing.";
        return false;
    }
    if (protocolValue->string != ModernProtocolVersion || HeaderValue(request.headers, "mcp-protocol-version") != protocolValue->string)
    {
        errorCode = -32020;
        diagnostics = "MCP-Protocol-Version header does not match request metadata.";
        return false;
    }
    std::string expectedName;
    if (method == "tools/call" || method == "prompts/get") expectedName = cld::JsonStringOr(*params, "name");
    else if (method == "resources/read") expectedName = cld::JsonStringOr(*params, "uri");
    if (!expectedName.empty() || method == "tools/call" || method == "prompts/get" || method == "resources/read")
    {
        std::string decodedName;
        const std::string nameHeader = HeaderValue(request.headers, "mcp-name");
        if (nameHeader.empty() || !DecodeHeaderValue(nameHeader, decodedName) || decodedName != expectedName)
        {
            errorCode = -32020;
            diagnostics = "Mcp-Name header is missing, malformed, or does not match the request body.";
            return false;
        }
    }
    return true;
}

void Server::PruneLegacySessionsLocked(std::chrono::steady_clock::time_point now)
{
    for (auto it = m_sessions.begin(); it != m_sessions.end();)
    {
        if (now - it->second.lastSeen >= LegacySessionIdleTimeout)
        {
            it = m_sessions.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool Server::ResolveSession(const HttpRequest& request, Session& session, std::string& diagnostics)
{
    const std::string sessionId = HeaderValue(request.headers, "mcp-session-id");
    if (sessionId.empty())
    {
        diagnostics = "MCP-Session-Id is required.";
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    PruneLegacySessionsLocked(now);
    const auto it = m_sessions.find(sessionId);
    if (it == m_sessions.end())
    {
        diagnostics = "Unknown MCP session.";
        return false;
    }
    it->second.lastSeen = now;
    session = it->second;
    return true;
}

void Server::MarkSessionInitialized(const HttpRequest& request)
{
    const std::string sessionId = HeaderValue(request.headers, "mcp-session-id");
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_sessions.find(sessionId);
    if (it != m_sessions.end())
    {
        it->second.initialized = true;
        it->second.lastSeen = std::chrono::steady_clock::now();
    }
}

void Server::AppendLog(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_recentRequests.push_back(message);
    if (m_recentRequests.size() > 12)
    {
        m_recentRequests.erase(m_recentRequests.begin(), m_recentRequests.begin() + (m_recentRequests.size() - 12));
    }
}

std::string GenerateToken(size_t byteCount)
{
    std::random_device randomDevice;
    std::ostringstream token;
    token << std::hex << std::setfill('0');
    for (size_t i = 0; i < byteCount; ++i)
    {
        token << std::setw(2) << (randomDevice() & 0xffu);
    }
    return token.str();
}

std::string AuthenticationModeName(AuthenticationMode mode)
{
    switch (mode)
    {
    case AuthenticationMode::None:
        return "none";
    case AuthenticationMode::BearerToken:
    default:
        return "bearer_token";
    }
}

AuthenticationMode AuthenticationModeFromName(
    const std::string& name,
    AuthenticationMode fallback)
{
    if (name == "bearer_token" || name == "bearer")
    {
        return AuthenticationMode::BearerToken;
    }
    if (name == "none")
    {
        return AuthenticationMode::None;
    }
    return fallback;
}

std::string AccessModeName(AccessMode mode)
{
    switch (mode)
    {
    case AccessMode::ReadOnly:
        return "read_only";
    case AccessMode::AllowMutations:
        return "allow_mutations";
    case AccessMode::ConfirmMutations:
    default:
        return "confirm_mutations";
    }
}

AccessMode AccessModeFromName(const std::string& name, AccessMode fallback)
{
    if (name == "read_only")
    {
        return AccessMode::ReadOnly;
    }
    if (name == "allow_mutations")
    {
        return AccessMode::AllowMutations;
    }
    if (name == "confirm_mutations")
    {
        return AccessMode::ConfirmMutations;
    }
    return fallback;
}

std::string BuildActionsSchemaJson()
{
    return R"json({
  "actions": [
    {"method":"set_scene","description":"Load a scene and optional environment map.","inputSchema":{"type":"object","properties":{"path":{"type":"string"},"scenePath":{"type":"string"},"environmentPath":{"type":"string"}},"additionalProperties":false}},
    {"method":"set_camera","description":"Set camera position, yaw/pitch/roll in radians, vertical FOV in degrees, and temporal history handling.","inputSchema":{"type":"object","properties":{"position":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"yaw":{"type":"number"},"pitch":{"type":"number"},"roll":{"type":"number"},"fovDegrees":{"type":"number","minimum":1,"maximum":179},"historyMode":{"type":"string","enum":["auto","preserve","reset"]}},"additionalProperties":false}},
    {"method":"set_material","description":"Override one material by index or name, including factors and texture slot overrides.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"baseColor":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"emissive":{"type":"array","items":{"type":"number"},"minItems":4,"maxItems":4},"roughness":{"type":"number"},"metallic":{"type":"number"},"occlusionStrength":{"type":"number"},"normalStrength":{"type":"number"},"alphaCutoff":{"type":"number"},"alphaMasked":{"type":"boolean"},"packedORM":{"type":"boolean"},"textures":{"type":"object","properties":{"baseColor":{"type":"string"},"normal":{"type":"string"},"roughness":{"type":"string"},"metallic":{"type":"string"},"occlusion":{"type":"string"},"emissive":{"type":"string"},"alpha":{"type":"string"}},"additionalProperties":false},"clearTextures":{"type":"array","items":{"oneOf":[{"type":"integer"},{"type":"string"}]}},"resetToSource":{"type":"boolean"}},"additionalProperties":false}},
    {"method":"set_material_texture","description":"Set, clear, or reset one material texture slot.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"slot":{"oneOf":[{"type":"integer"},{"type":"string"}]},"path":{"type":"string"},"clear":{"type":"boolean"},"resetToSource":{"type":"boolean"}},"required":["slot"],"additionalProperties":false}},
    {"method":"reset_material","description":"Reset one material back to imported source values and textures.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"}},"additionalProperties":false}},
    {"method":"save_material_variant","description":"Save or replace a named per-material variant snapshot.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"variant":{"type":"string"},"variantName":{"type":"string"}},"additionalProperties":false}},
    {"method":"apply_material_variant","description":"Apply a named or indexed material variant.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"variant":{"type":"string"},"variantName":{"type":"string"},"variantIndex":{"type":"integer"}},"additionalProperties":false}},
    {"method":"delete_material_variant","description":"Delete a named or indexed material variant.","inputSchema":{"type":"object","properties":{"index":{"type":"integer"},"name":{"type":"string"},"variant":{"type":"string"},"variantName":{"type":"string"},"variantIndex":{"type":"integer"}},"additionalProperties":false}},
    {"method":"set_material_view","description":"Set selected material and material focus display mode.","inputSchema":{"type":"object","properties":{"selectedMaterial":{"type":"integer"},"focusMode":{"type":"string","enum":["normal","isolate","dim"]}},"additionalProperties":false}},
    {"method":"set_color_management","description":"Set final view exposure, gamma, and tone mapper.","inputSchema":{"type":"object","properties":{"exposure":{"type":"number"},"gamma":{"type":"number"},"toneMapper":{"type":"string","enum":["none","reinhard","aces"]}},"additionalProperties":false}},
    {"method":"set_lighting","description":"Set direct, sky, environment, and light sampling controls.","inputSchema":{"type":"object","properties":{"direction":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"color":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"intensity":{"type":"number"},"rayTMin":{"type":"number"},"skyEnabled":{"type":"boolean"},"skyColor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"skyHorizonColor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"skyZenithColor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"skyGroundColor":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"skyIntensity":{"type":"number"},"sunIntensity":{"type":"number"},"sunSize":{"type":"number"},"environmentEnabled":{"type":"boolean"},"environmentIntensity":{"type":"number"},"environmentRotation":{"type":"number"},"sunNEE":{"type":"boolean"},"skyNEE":{"type":"boolean"},"emissiveTriangleLights":{"type":"boolean"},"emissiveIntensity":{"type":"number"},"proceduralAreaLights":{"type":"boolean"},"areaLightIntensity":{"type":"number"}},"additionalProperties":false}},
    {"method":"set_path_tracing","description":"Set path tracing mode and sampling controls.","inputSchema":{"type":"object","properties":{"mode":{"type":"string","enum":["baseline","path_tracing","restir_gi","restir_di","restir_gi_di","combined","restir_pt","restir_pt_di"]},"samplesPerFrame":{"type":"integer"},"maxBounces":{"type":"integer"},"minBounces":{"type":"integer"},"radianceClamp":{"type":"number"},"temporalClamp":{"type":"number"},"maxAccumSamples":{"type":"integer"},"freezeAccumulation":{"type":"boolean"},"adaptiveSampling":{"type":"boolean"},"maxAdaptiveSPP":{"type":"integer"},"varianceThreshold":{"type":"number"},"disocclusionBoost":{"type":"number"}},"additionalProperties":false}},
    {"method":"set_quality","description":"Select a renderer quality profile and configure ray, resolution, and temporal budgets.","inputSchema":{"type":"object","properties":{"qualityProfile":{"type":"string","enum":["interactive_game","sharp_preview","reference_still"]},"restirBackend":{"type":"string","enum":["rtxdi","off"]},"secondaryShadingRate":{"type":"string","enum":["auto","full","adaptive_half"]},"resolutionMode":{"type":"string","enum":["native","fixed","dynamic"]},"fixedRenderScale":{"type":"number","minimum":0.25,"maximum":1},"minRenderScale":{"type":"number","minimum":0.25,"maximum":1},"maxRenderScale":{"type":"number","minimum":0.25,"maximum":1},"rayBudget":{"type":"object","properties":{"movingSpp":{"type":"integer","minimum":1,"maximum":8},"movingBounces":{"type":"integer","minimum":1,"maximum":16},"staticBaseSpp":{"type":"integer","minimum":1,"maximum":8},"staticMaxSpp":{"type":"integer","minimum":1,"maximum":16},"staticBounces":{"type":"integer","minimum":1,"maximum":16},"settleFrames":{"type":"integer","minimum":0,"maximum":120},"targetGpuMs":{"type":"number","minimum":1,"maximum":100}},"additionalProperties":false},"finalTaa":{"type":"boolean"},"sharpenStrength":{"type":"number","minimum":0,"maximum":1},"referenceSpp":{"type":"integer","minimum":1,"maximum":1048576}},"additionalProperties":false}},
    {"method":"set_restir","description":"Set GI/DI ReSTIR reuse and reservoir controls.","inputSchema":{"type":"object","properties":{"temporalReuse":{"type":"boolean"},"spatialReusePasses":{"type":"integer"},"spatialRadius":{"type":"integer"},"candidateSamples":{"type":"integer"},"mClamp":{"type":"number"},"diTemporalReuse":{"type":"boolean"},"diSpatialReusePasses":{"type":"integer"},"diCandidateSamples":{"type":"integer"},"diMClamp":{"type":"number"},"reservoirReprojection":{"type":"boolean"},"reservoirValidation":{"type":"boolean"},"giValidationRay":{"type":"boolean"},"reservoirMaxAge":{"type":"integer"}},"additionalProperties":false}},
    {"method":"set_denoise","description":"Set denoiser backend, NRD/DLSS mode, temporal stability, jitter, and realtime reconstruction controls.","inputSchema":{"type":"object","properties":{"preset":{"type":"string","enum":["interactive_stable","sharp_preview","still_capture"]},"backend":{"type":"string","enum":["internal","nrd_reblur","nrd_relax","dlss_rr","off"]},"dlssMode":{"type":"string","enum":["quality","balanced","performance","ultra_performance"]},"dlssEnabledWhenAvailable":{"type":"boolean"},"resetDlss":{"type":"boolean"},"resetNrd":{"type":"boolean"},"enabled":{"type":"boolean"},"splitSignalDenoise":{"type":"boolean"},"realtimeReconstruction":{"type":"boolean"},"cameraJitter":{"type":"boolean"},"temporalStability":{"type":"boolean"},"jitterMode":{"type":"string","enum":["stable32","stable16","halton","off"]},"movingJitterScale":{"type":"number"},"resetHistory":{"type":"boolean"},"maxHistoryFrames":{"type":"integer"},"temporalAlphaMin":{"type":"number"},"temporalAlphaMax":{"type":"number"},"historyClampSigma":{"type":"number"},"reactiveThreshold":{"type":"number"},"specularHistoryScale":{"type":"number"},"spatialIterations":{"type":"integer","deprecated":true,"description":"Legacy alias retained for one schema version; prefer atrousPasses."},"atrousPasses":{"type":"integer"},"diffuseFilterStrength":{"type":"number"},"specularFilterStrength":{"type":"number"},"varianceScale":{"type":"number"},"normalSigma":{"type":"number"},"depthSigma":{"type":"number"},"luminanceSigma":{"type":"number"},"albedoSigma":{"type":"number"},"strength":{"type":"number"}},"additionalProperties":false}},
    {"method":"set_view","description":"Set active debug view and display toggles.","inputSchema":{"type":"object","properties":{"debugView":{"type":"integer","minimum":0,"maximum":53},"normalMapYFlip":{"type":"boolean"},"environmentEnabled":{"type":"boolean"}},"additionalProperties":false}}
  ]
})json";
}

std::string BuildToolsListJson()
{
    const char* actionSchema = R"json({"type":"object","properties":{"method":{"type":"string"},"params":{"type":"object"}},"required":["method","params"],"additionalProperties":false})json";
    const char* setCameraSpeedSchema = R"json({"type":"object","properties":{"baseMoveSpeed":{"type":"number"},"fastMoveSpeed":{"type":"number"}},"additionalProperties":false})json";
    const char* fitCameraSchema = R"json({"type":"object","properties":{"padding":{"type":"number"},"preserveOrientation":{"type":"boolean"},"yaw":{"type":"number"},"pitch":{"type":"number"}},"additionalProperties":false})json";
    const char* displayResolutionSchema = R"json({"type":"object","properties":{"preset":{"type":"string","enum":["720p","1080p","4k"]},"width":{"type":"integer"},"height":{"type":"integer"}},"additionalProperties":false})json";
    const char* projectPathSchema = R"json({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})json";
    const char* runActionsSchema = R"json({"type":"object","properties":{"actions":{"type":"array","items":{"type":"object","properties":{"method":{"type":"string"},"params":{"type":"object"}},"required":["method","params"],"additionalProperties":false},"minItems":1,"maxItems":16},"validateOnly":{"type":"boolean"},"stopOnError":{"type":"boolean"}},"required":["actions"],"additionalProperties":false})json";
    const char* captureDebugPackSchema = R"json({"type":"object","properties":{"views":{"type":"array","items":{"oneOf":[{"type":"integer"},{"type":"string"}]},"maxItems":8},"restoreView":{"type":"boolean"}},"additionalProperties":false})json";
    std::vector<std::string> tools;
    tools.push_back(ToolJson("lookdevpt.get_stats", "Get renderer stats", "Return DXR, scene, renderer, ReSTIR, and denoiser stats.", EmptyObjectSchema().c_str(), true));
    tools.push_back(ToolJson("lookdevpt.get_state", "Get renderer state", "Return current scene, camera, lighting, path tracing, ReSTIR, denoise, and view state.", EmptyObjectSchema().c_str(), true));
    tools.push_back(ToolJson("lookdevpt.list_materials", "List materials", "Return material names, usage counts, editable factors, and texture slot state.", EmptyObjectSchema().c_str(), true));
    tools.push_back(ToolJson("lookdevpt.list_debug_views", "List debug views", "Return debug view ids, labels, and keys.", EmptyObjectSchema().c_str(), true));
    tools.push_back(ToolJson("lookdevpt.list_render_modes", "List render modes", "Return path tracing mode labels and action values.", EmptyObjectSchema().c_str(), true));
    tools.push_back(ToolJson("lookdevpt.get_diagnostics", "Get diagnostics", "Return scene, project, capture, and MCP diagnostics.", EmptyObjectSchema().c_str(), true));
    tools.push_back(ToolJson("lookdevpt.capture_viewport", "Capture viewport", "Capture the current path-traced viewport as PNG.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.capture_debug_pack", "Capture debug pack", "Capture up to eight debug views as PNG resources.", captureDebugPackSchema));
    tools.push_back(ToolJson("lookdevpt.validate_action", "Validate action", "Validate an action without applying it.", actionSchema, true));
    tools.push_back(ToolJson("lookdevpt.run_actions", "Run actions", "Validate and run multiple action-layer mutations as one MCP request.", runActionsSchema));
    tools.push_back(ToolJson("lookdevpt.reset_accumulation", "Reset accumulation", "Reset progressive accumulation.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.reset_denoise_history", "Reset denoise history", "Invalidate denoise temporal history.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.reset_reservoirs", "Reset reservoirs", "Reset accumulation, ReSTIR reservoirs, and denoise history.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.reset_camera_view", "Reset camera view", "Return the camera to the default scene view.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.set_camera_speed", "Set camera speed", "Set WASD base and fast camera speeds.", setCameraSpeedSchema));
    tools.push_back(ToolJson("lookdevpt.fit_camera_to_scene", "Fit camera to scene", "Move the camera so the current scene bounds fit in view.", fitCameraSchema));
    tools.push_back(ToolJson("lookdevpt.set_display_resolution", "Set display resolution", "Resize the application viewport/window to a preset or custom resolution.", displayResolutionSchema));
    tools.push_back(ToolJson("lookdevpt.load_project", "Load project", "Load a local .lookdevpt.json project file.", projectPathSchema));
    tools.push_back(ToolJson("lookdevpt.save_project", "Save project", "Save the current project path.", EmptyObjectSchema().c_str()));
    tools.push_back(ToolJson("lookdevpt.save_project_as", "Save project as", "Save the project to a local .lookdevpt.json path.", projectPathSchema));

    const std::string actions = BuildActionsSchemaJson();
    cld::JsonValue root = cld::JsonParser(actions).Parse();
    const cld::JsonValue* actionArray = cld::FindMember(root, "actions");
    if (actionArray && actionArray->type == cld::JsonValue::Type::Array)
    {
        for (const cld::JsonValue& action : actionArray->array)
        {
            const std::string method = cld::JsonStringOr(action, "method");
            const cld::JsonValue* inputSchema = cld::FindMember(action, "inputSchema");
            const std::string description = cld::JsonStringOr(action, "description");
            if (!method.empty() && inputSchema)
            {
                const std::string toolName = "lookdevpt." + method;
                tools.push_back(ToolJson(toolName.c_str(), method.c_str(), description.c_str(), cld::JsonValueToJson(*inputSchema).c_str()));
            }
        }
    }

    std::ostringstream out;
    out << "{\"tools\":[";
    AppendJoined(out, tools);
    out << "]}";
    return out.str();
}

std::string BuildResourcesListJson()
{
    std::vector<std::string> resources;
    resources.push_back(ResourceJson("lookdevpt://state", "state", "D3D12LookDevPT State", "Current renderer state as JSON.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://stats", "stats", "D3D12LookDevPT Stats", "Current renderer stats as JSON.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://diagnostics", "diagnostics", "Diagnostics", "Scene, project, capture, and MCP diagnostics.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://materials", "materials", "Materials", "Scene material list as JSON.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://material-variants", "material_variants", "Material Variants", "Saved per-material variant snapshots.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://material-presets", "material_presets", "Material Presets", "Built-in and user material presets.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://debug-views", "debug_views", "Debug Views", "Available debug view ids and labels.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://render-modes", "render_modes", "Render Modes", "Available render modes and action values.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://project", "project", "Project", "Current project path and dirty state.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://scene/summary", "scene_summary", "Scene Summary", "Loaded scene counts, bounds, lights, and asset paths.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://actions/schema", "actions_schema", "Action Schema", "Action names and input schemas.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://captures/index", "captures_index", "Capture Index", "In-memory MCP capture history.", "application/json"));
    resources.push_back(ResourceJson("lookdevpt://captures/latest.png", "latest_capture", "Latest Viewport Capture", "Most recent viewport capture.", "image/png"));
    std::ostringstream out;
    out << "{\"resources\":[";
    AppendJoined(out, resources);
    out << "]}";
    return out.str();
}

std::string BuildResourceTemplatesListJson()
{
    std::vector<std::string> templates;
    templates.push_back(ResourceTemplateJson("lookdevpt://captures/{id}.png", "capture_by_id", "Capture By Id", "Read an in-memory PNG capture by id.", "image/png"));
    templates.push_back(ResourceTemplateJson("lookdevpt://materials/{index}", "material_by_index", "Material By Index", "Read one material JSON object by material index.", "application/json"));
    templates.push_back(ResourceTemplateJson("lookdevpt://materials/{index}/textures", "material_textures_by_index", "Material Textures By Index", "Read texture slots and override state for one material.", "application/json"));
    std::ostringstream out;
    out << "{\"resourceTemplates\":[";
    AppendJoined(out, templates);
    out << "]}";
    return out.str();
}

std::string BuildPromptsListJson()
{
    std::vector<std::string> prompts;
    prompts.push_back(PromptJson("lookdevpt.inspect_scene", "Inspect Scene", "Inspect the current renderer state, stats, materials, and diagnostics."));
    prompts.push_back(PromptJson("lookdevpt.tune_denoise", "Tune Denoise", "Tune temporal denoise settings for a stable LookDev viewport."));
    prompts.push_back(PromptJson("lookdevpt.setup_camera_shot", "Setup Camera Shot", "Create or refine a camera shot using scene bounds and current state."));
    prompts.push_back(PromptJson("lookdevpt.capture_debug_review", "Capture Debug Review", "Capture key debug views and summarize rendering issues."));
    std::ostringstream out;
    out << "{\"prompts\":[";
    AppendJoined(out, prompts);
    out << "]}";
    return out.str();
}

std::string BuildPromptGetResultJson(const std::string& name, const cld::JsonValue* arguments, bool& found)
{
    (void)arguments;
    found = true;
    if (name == "lookdevpt.inspect_scene")
    {
        return PromptTextResult("Inspect the active D3D12LookDevPT scene.",
            "Inspect the current D3D12LookDevPT session. First call lookdevpt.get_state, lookdevpt.get_stats, lookdevpt.list_materials, and lookdevpt.get_diagnostics. Then summarize scene scale, camera position, render mode, denoise state, material count, active lights, and any obvious setup issues. Do not mutate state.");
    }
    if (name == "lookdevpt.tune_denoise")
    {
        return PromptTextResult("Tune denoise settings for stable LookDev.",
            "Tune the D3D12LookDevPT denoise settings for a stable interactive viewport. Read state/stats first, validate the proposed set_denoise parameters with lookdevpt.validate_action, then apply lookdevpt.set_denoise only if mutation access is allowed. Prefer interactive_stable for camera work and still_capture for static review. Confirm the result with lookdevpt.get_state.");
    }
    if (name == "lookdevpt.setup_camera_shot")
    {
        return PromptTextResult("Set up a camera shot.",
            "Set up a camera shot in D3D12LookDevPT. Read lookdevpt://scene/summary and lookdevpt.get_state, optionally call lookdevpt.fit_camera_to_scene, then use lookdevpt.set_camera for final position/yaw/pitch. Confirm with lookdevpt.get_state and capture the viewport if requested.");
    }
    if (name == "lookdevpt.capture_debug_review")
    {
        return PromptTextResult("Capture debug views for review.",
            "Capture a debug review pack from D3D12LookDevPT. Call lookdevpt.capture_debug_pack with Final, Base Color, World Normal, Roughness, Metallic, Direct Signal, Indirect Signal, and History Confidence. Then read lookdevpt://captures/index and summarize visible render/debug issues.");
    }
    found = false;
    return "{}";
}
}

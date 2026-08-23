#include "McpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
    constexpr const char* Token = "mcp-test-token";

    class FakeHost final : public mcp::IServerHost
    {
    public:
        mcp::ToolResult CallMcpTool(
            const std::string& name,
            const cld::JsonValue&,
            int,
            mcp::ToolCallAuthorization authorization) override
        {
            lastOneTimeAuthorization.store(
                authorization.oneTimeMutationGrant);
            mcp::ToolResult result;
            result.ok = true;
            result.text = "called " + name;
            result.structuredJson = "{\"ok\":true,\"name\":\"" + cld::EscapeJson(name) + "\"}";
            if (name == "lookdevpt.capture_viewport")
            {
                result.structuredJson = "{\"ok\":true,\"captureId\":1,\"resource\":\"lookdevpt://captures/1.png\"}";
                result.contentJson = "[{\"type\":\"text\",\"text\":\"fake viewport capture\"},"
                    "{\"type\":\"image\",\"data\":\"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=\",\"mimeType\":\"image/png\"},"
                    "{\"type\":\"resource_link\",\"uri\":\"lookdevpt://captures/1.png\",\"name\":\"capture_1\",\"mimeType\":\"image/png\"}]";
            }
            else if (name == "lookdevpt.start_review")
            {
                result.structuredJson = "{\"ok\":true,\"reviewId\":1,\"state\":\"running\",\"resource\":\"lookdevpt://reviews/1\"}";
            }
            else if (name == "lookdevpt.get_review")
            {
                result.structuredJson = "{\"ok\":true,\"reviewId\":1,\"state\":\"completed\",\"progress\":1,\"capture\":\"lookdevpt://captures/1.png\",\"heatmap\":\"lookdevpt://comparisons/1/heatmap.png\"}";
            }
            else if (name == "lookdevpt.create_checkpoint")
            {
                result.structuredJson = "{\"ok\":true,\"checkpointId\":1,\"sceneFingerprint\":\"scene-1\",\"resource\":\"lookdevpt://checkpoints/1\"}";
            }
            else if (name == "lookdevpt.run_actions")
            {
                result.structuredJson = "{\"ok\":true,\"validateOnly\":true,\"appliedCount\":0}";
            }
            else if (name == "lookdevpt.restore_checkpoint" || name == "lookdevpt.delete_checkpoint")
            {
                result.structuredJson = "{\"ok\":true,\"checkpointId\":1}";
            }
            else if (name == "lookdevpt.start_benchmark")
            {
                result.structuredJson = "{\"ok\":true,\"benchmarkId\":1,\"state\":\"running\",\"resource\":\"lookdevpt://benchmarks/1\"}";
            }
            else if (name == "lookdevpt.get_benchmark")
            {
                result.structuredJson = "{\"ok\":true,\"benchmarkId\":1,\"state\":\"completed\",\"progress\":1}";
            }
            else if (name == "lookdevpt.cancel_benchmark")
            {
                result.structuredJson = "{\"ok\":true,\"benchmarkId\":1,\"state\":\"cancelling\"}";
            }
            return result;
        }

        mcp::ResourceResult ReadMcpResource(const std::string& uri) override
        {
            mcp::ResourceResult result;
            result.uri = uri;
            if (uri == "lookdevpt://captures/1.png" || uri == "lookdevpt://captures/latest.png" ||
                uri == "lookdevpt://comparisons/1/heatmap.png")
            {
                result.ok = true;
                result.mimeType = "image/png";
                result.blob = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";
                return result;
            }
            if (uri == "lookdevpt://reviews/1")
            {
                result.ok = true;
                result.text = "{\"reviewId\":1,\"state\":\"completed\",\"progress\":1}";
                return result;
            }
            if (uri == "lookdevpt://comparisons/1")
            {
                result.ok = true;
                result.text = "{\"id\":1,\"rmse\":0.01,\"psnr\":40,\"ssim\":0.99}";
                return result;
            }
            if (uri != "lookdevpt://state")
            {
                result.error = "Resource not found.";
                return result;
            }
            result.ok = true;
            result.text = "{\"uri\":\"" + cld::EscapeJson(uri) + "\"}";
            return result;
        }

        size_t PendingMcpCommandCount() const override
        {
            return 0;
        }

        std::atomic_bool lastOneTimeAuthorization = false;
    };

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::string NestedArray(std::size_t containerDepth)
    {
        return std::string(containerDepth, '[') + "0" +
            std::string(containerDepth, ']');
    }

    std::string Utf8Bytes(
        std::initializer_list<unsigned char> bytes)
    {
        std::string result;
        result.reserve(bytes.size());
        for (unsigned char byte : bytes)
        {
            result.push_back(static_cast<char>(byte));
        }
        return result;
    }

    bool JsonParseFails(const std::string& json)
    {
        try
        {
            (void)cld::JsonParser(json).Parse();
            return false;
        }
        catch (const std::runtime_error&)
        {
            return true;
        }
    }

    void TestJsonDepthLimit()
    {
        const cld::JsonValue boundary =
            cld::JsonParser(NestedArray(64)).Parse();
        Require(
            boundary.type == cld::JsonValue::Type::Array,
            "JSON parser rejected the maximum container depth");

        bool rejected = false;
        try
        {
            (void)cld::JsonParser(NestedArray(65)).Parse();
        }
        catch (const std::runtime_error& error)
        {
            rejected = std::string(error.what()) ==
                "JSON nesting depth exceeds the limit.";
        }
        Require(
            rejected,
            "JSON parser accepted a container beyond the depth limit");
    }

    void TestJsonUnicodeStrings()
    {
        const std::string japanese = Utf8Bytes({
            0xe6, 0x97, 0xa5,
            0xe6, 0x9c, 0xac,
            0xe8, 0xaa, 0x9e });
        const std::string emoji = Utf8Bytes({ 0xf0, 0x9f, 0x98, 0x80 });
        const std::string rawJson =
            "{\"label\":\"" + japanese + emoji + "\"}";
        const std::string escapedJson =
            R"json({"label":"\u65e5\u672c\u8a9e\ud83d\ude00"})json";
        const cld::JsonValue raw = cld::JsonParser(rawJson).Parse();
        const cld::JsonValue escaped =
            cld::JsonParser(escapedJson).Parse();
        Require(
            cld::JsonStringOr(raw, "label") == japanese + emoji &&
                cld::JsonStringOr(escaped, "label") == japanese + emoji,
            "JSON unicode escape did not decode to UTF-8");
        Require(
            mcp::CanonicalArgumentsSha256(raw) ==
                mcp::CanonicalArgumentsSha256(escaped),
            "raw and escaped unicode produced different argument hashes");

        Require(
            JsonParseFails(R"json({"x":"\ud83d"})json") &&
                JsonParseFails(R"json({"x":"\ud83d\u0041"})json") &&
                JsonParseFails(R"json({"x":"\ude00"})json"),
            "JSON parser accepted a malformed unicode surrogate");
        Require(
            JsonParseFails("{\"x\":\"line\nbreak\"}"),
            "JSON parser accepted a raw control character");

        std::string invalidUtf8 = "{\"x\":\"";
        invalidUtf8 += Utf8Bytes({ 0xc0, 0xaf });
        invalidUtf8 += "\"}";
        std::string surrogateUtf8 = "{\"x\":\"";
        surrogateUtf8 += Utf8Bytes({ 0xed, 0xa0, 0x80 });
        surrogateUtf8 += "\"}";
        std::string outOfRangeUtf8 = "{\"x\":\"";
        outOfRangeUtf8 += Utf8Bytes({ 0xf4, 0x90, 0x80, 0x80 });
        outOfRangeUtf8 += "\"}";
        Require(
            JsonParseFails(invalidUtf8) &&
                JsonParseFails(surrogateUtf8) &&
                JsonParseFails(outOfRangeUtf8),
            "JSON parser accepted invalid raw UTF-8");
    }

    void TestJsonNumberGrammarAndLocale()
    {
        const cld::JsonValue localizedDecimal =
            cld::JsonParser(" \t\r\n1.5 \n").Parse();
        Require(
            localizedDecimal.type == cld::JsonValue::Type::Number &&
                localizedDecimal.number == 1.5,
            "JSON parser rejected an RFC 8259 number or whitespace");

        const std::array<const char*, 12> invalidNumbers = {
            "01", "-01", "00", "-", ".1", "1.",
            "1e", "1e+", "+1", "NaN", "Infinity", "1e309"
        };
        for (const char* invalid : invalidNumbers)
        {
            Require(
                JsonParseFails(invalid),
                "JSON parser accepted invalid number grammar");
        }
        Require(
            JsonParseFails("\f0") && JsonParseFails("\v0"),
            "JSON parser accepted non-RFC whitespace");

        const char* currentLocale = std::setlocale(LC_NUMERIC, nullptr);
        const std::string originalLocale =
            currentLocale != nullptr ? currentLocale : "C";
        const char* changedLocale =
            std::setlocale(LC_NUMERIC, "German_Germany.1252");
        if (changedLocale == nullptr)
        {
            changedLocale = std::setlocale(LC_NUMERIC, "de-DE");
        }
        bool localeIndependent = true;
        if (changedLocale != nullptr)
        {
            try
            {
                const cld::JsonValue value =
                    cld::JsonParser("1.5").Parse();
                localeIndependent =
                    value.type == cld::JsonValue::Type::Number &&
                    value.number == 1.5 && JsonParseFails("1,5");
            }
            catch (const std::runtime_error&)
            {
                localeIndependent = false;
            }
        }
        (void)std::setlocale(LC_NUMERIC, originalLocale.c_str());
        Require(
            localeIndependent,
            "JSON number parsing changed with the process locale");
    }

    void TestCanonicalArgumentFixtures()
    {
        const auto requireHash = [](
            const std::string& json,
            const char* expected,
            const char* diagnostics)
        {
            const cld::JsonValue value = cld::JsonParser(json).Parse();
            Require(
                mcp::CanonicalArgumentsSha256(value) == expected,
                diagnostics);
        };

        const std::string japanese = Utf8Bytes({
            0xe6, 0x97, 0xa5,
            0xe6, 0x9c, 0xac,
            0xe8, 0xaa, 0x9e });
        const std::string emoji = Utf8Bytes({ 0xf0, 0x9f, 0x98, 0x80 });
        const std::string privateUse = Utf8Bytes({ 0xee, 0x80, 0x80 });
        requireHash(
            "{}",
            "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a",
            "empty canonical argument hash changed");
        requireHash(
            "{\"b\":1,\"a\":-0}",
            "2f954b957c86ae054ee0935643ad1f0dd7522789a6490bd06a116978447b012b",
            "sorted canonical argument hash changed");
        Require(
            mcp::CanonicalArgumentsJson(
                cld::JsonParser("{\"b\":1,\"a\":-0}").Parse()) ==
                "{\"a\":0,\"b\":1e0}",
            "canonical argument JSON changed");
        requireHash(
            "{\"text\":\"line\\n" + japanese +
                "\",\"values\":[true,null,0.5]}",
            "bc39897afca6235bae005c4ac4eee62f819ab375661018e5e5c500da29712546",
            "unicode canonical argument hash changed");
        requireHash(
            "{\"tiny\":1e-8,\"large\":1e20,\"small\":1e-7}",
            "ffd94ad98d15b3ef7f3ba1bd7c3cebd60062be66388df869d700b60de5f5504c",
            "exponent canonical argument hash changed");
        requireHash(
            "{\"" + emoji + "\":2,\"" + privateUse + "\":1}",
            "0c17a92c27cd16a347789cf9fe4b62e7d020ae5e2c3d8371cf43ac41f8ed30ef",
            "UTF-8 key ordering canonical hash changed");

        const auto requireNumberHash = [](
            double number,
            const char* expected,
            const char* diagnostics)
        {
            cld::JsonValue value;
            value.type = cld::JsonValue::Type::Object;
            cld::JsonValue member;
            member.type = cld::JsonValue::Type::Number;
            member.number = number;
            value.object.emplace("v", member);
            Require(
                mcp::CanonicalArgumentsSha256(value) == expected,
                diagnostics);
        };
        requireNumberHash(
            1e6,
            "ce11dcd09748a235500ecf8323fa48697351a2ba66a557ebc5b3478993706729",
            "1e6 canonical argument hash changed");
        requireNumberHash(
            1e15,
            "363ed7a10f2aaa15a2dfbd110ff2cd57f46ac77b5ee3834d1a2fd73f93490c88",
            "1e15 canonical argument hash changed");
        requireNumberHash(
            1e16,
            "80c8d25e04ccc932a94aa6fdcc0c90ee36f23665b313b0a3dad06fe333f8d606",
            "1e16 canonical argument hash changed");
        requireNumberHash(
            1e17,
            "031294a3c1bd126b83c6e02a51cb3accdb3e39b5ec31da5f585d3b655c1374ae",
            "1e17 canonical argument hash changed");
        requireNumberHash(
            1e-4,
            "9c3f563e731898097d62d45d3c9cab62b7804c3b0e3fd3892a3cff32a222e475",
            "1e-4 canonical argument hash changed");
        requireNumberHash(
            1e-5,
            "d87d46e7098aedb95fb6cca01d36d8da61136e1f5a51291916cb5abf7262f5b1",
            "1e-5 canonical argument hash changed");
        requireNumberHash(
            std::nextafter(1.0, 0.0),
            "490716aaf54aa8133e2ee995bc7dcd6e80caa0ab18aa5d262731ae0eb7a1c939",
            "double below-one boundary canonical hash changed");
        requireNumberHash(
            std::nextafter(1.0, 2.0),
            "c9d17a945f8b692b317d3d7b86dd11afafdace37e8e20ebbac3596043312d1bc",
            "double above-one boundary canonical hash changed");
        requireNumberHash(
            (std::numeric_limits<double>::max)(),
            "ee16b8e5a24a3e4a3e566dc4da9a4afa993274e80d3744e71987b3bd32312f50",
            "maximum double canonical hash changed");
        requireNumberHash(
            (std::numeric_limits<double>::denorm_min)(),
            "e78ddea1a7937dbd43be5cceeae312be7fb38ccf3b908835ac066e6174b658b7",
            "minimum subnormal canonical hash changed");
        requireNumberHash(
            -1e6,
            "be4dcb04db2f33d2433b16a150d26802dad440a10f474e378b542eff8da5b21b",
            "negative 1e6 canonical argument hash changed");
    }

    void TestToolCatalogReadOnlyAnnotations()
    {
        const cld::JsonValue catalog =
            cld::JsonParser(mcp::BuildToolsListJson()).Parse();
        const cld::JsonValue* tools = cld::FindMember(catalog, "tools");
        Require(
            tools != nullptr &&
                tools->type == cld::JsonValue::Type::Array,
            "tool catalog did not contain a tools array");
        const cld::JsonValue* captureViewport = nullptr;
        for (const cld::JsonValue& tool : tools->array)
        {
            if (cld::JsonStringOr(tool, "name") ==
                "lookdevpt.capture_viewport")
            {
                captureViewport = &tool;
                break;
            }
        }
        Require(captureViewport != nullptr,
            "capture_viewport tool was missing from the catalog");
        const cld::JsonValue* annotations =
            cld::FindMember(*captureViewport, "annotations");
        Require(
            annotations != nullptr &&
                cld::JsonBoolOr(
                    *annotations, "readOnlyHint", false),
            "capture_viewport was not marked read-only");
    }

    std::string Meta(const std::string& approvalToken = {})
    {
        std::string meta = "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
            "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"mcp-server-tests\",\"version\":\"1.0\"},"
            "\"io.modelcontextprotocol/clientCapabilities\":{}";
        if (!approvalToken.empty())
        {
            meta += ",\"shaderjp.lookdevpt/approvalToken\":\"" +
                cld::EscapeJson(approvalToken) + "\"";
        }
        return meta + "}";
    }

    SOCKET Connect(uint16_t port)
    {
        SOCKET socketValue = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(socketValue != INVALID_SOCKET, "socket creation failed");
        DWORD timeoutMs = 5000;
        setsockopt(socketValue, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        setsockopt(socketValue, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(socketValue, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        {
            closesocket(socketValue);
            throw std::runtime_error("connect failed");
        }
        return socketValue;
    }

    void SendAll(SOCKET socketValue, const std::string& bytes)
    {
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const int sent = send(socketValue, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
            Require(sent > 0, "send failed");
            offset += static_cast<size_t>(sent);
        }
    }

    std::string ReceiveUntilClosed(SOCKET socketValue)
    {
        std::string result;
        char buffer[4096];
        for (;;)
        {
            const int received = recv(socketValue, buffer, sizeof(buffer), 0);
            if (received == 0) break;
            if (received < 0)
            {
                throw std::runtime_error("receive failed or timed out");
            }
            result.append(buffer, buffer + received);
        }
        return result;
    }

    std::string ReceiveUntil(SOCKET socketValue, const std::string& marker)
    {
        std::string result;
        char buffer[4096];
        while (result.find(marker) == std::string::npos)
        {
            const int received = recv(socketValue, buffer, sizeof(buffer), 0);
            if (received <= 0)
            {
                throw std::runtime_error("stream closed before expected marker");
            }
            result.append(buffer, buffer + received);
        }
        return result;
    }

    std::string RequestText(uint16_t port, const std::string& method, const std::string& body,
        const std::string& protocolVersion, const std::string& mcpMethod = {}, const std::string& mcpName = {},
        const std::string& sessionId = {}, const std::string& origin = {}, const std::string& token = Token)
    {
        std::string request = method + " /mcp HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
        if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
        if (!origin.empty()) request += "Origin: " + origin + "\r\n";
        if (!protocolVersion.empty()) request += "MCP-Protocol-Version: " + protocolVersion + "\r\n";
        if (!mcpMethod.empty()) request += "Mcp-Method: " + mcpMethod + "\r\n";
        if (!mcpName.empty()) request += "Mcp-Name: " + mcpName + "\r\n";
        if (!sessionId.empty()) request += "MCP-Session-Id: " + sessionId + "\r\n";
        request += "Accept: application/json, text/event-stream\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        request += body;
        return request;
    }

    std::string PairingRequestText(uint16_t port, const std::string& method, const std::string& path,
        const std::string& body = {}, const std::string& origin = {})
    {
        std::string request = method + " " + path + " HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
        if (!origin.empty()) request += "Origin: " + origin + "\r\n";
        request += "Accept: application/json\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        return request;
    }

    std::string JsonStringField(const std::string& response, const std::string& name)
    {
        const std::string prefix = "\"" + name + "\":\"";
        const size_t begin = response.find(prefix);
        if (begin == std::string::npos) return {};
        const size_t valueBegin = begin + prefix.size();
        const size_t end = response.find('"', valueBegin);
        return end == std::string::npos ? std::string{} : response.substr(valueBegin, end - valueBegin);
    }

    std::string ChunkedRequestText(uint16_t port, const std::string& body,
        const std::string& protocolVersion, const std::string& mcpMethod, const std::string& mcpName = {})
    {
        std::string request = "POST /mcp HTTP/1.1\r\n";
        request += "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";
        request += "Authorization: Bearer " + std::string(Token) + "\r\n";
        request += "MCP-Protocol-Version: " + protocolVersion + "\r\n";
        request += "Mcp-Method: " + mcpMethod + "\r\n";
        if (!mcpName.empty()) request += "Mcp-Name: " + mcpName + "\r\n";
        request += "Accept: application/json, text/event-stream\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Transfer-Encoding: chunked\r\n\r\n";

        const size_t firstSize = body.size() / 2;
        const std::array<std::string, 2> chunks = { body.substr(0, firstSize), body.substr(firstSize) };
        for (size_t index = 0; index < chunks.size(); ++index)
        {
            std::ostringstream size;
            size << std::hex << chunks[index].size();
            request += size.str();
            if (index == 0) request += ";client=test";
            request += "\r\n" + chunks[index] + "\r\n";
        }
        request += "0\r\n\r\n";
        return request;
    }

    std::string Exchange(uint16_t port, const std::string& request)
    {
        SOCKET socketValue = Connect(port);
        SendAll(socketValue, request);
        const std::string response = ReceiveUntilClosed(socketValue);
        closesocket(socketValue);
        return response;
    }

    std::string HeaderValueFromResponse(const std::string& response, const std::string& name)
    {
        const std::string prefix = name + ": ";
        const size_t start = response.find(prefix);
        if (start == std::string::npos) return {};
        const size_t valueStart = start + prefix.size();
        const size_t end = response.find("\r\n", valueStart);
        return response.substr(valueStart, end - valueStart);
    }

    uint16_t StartServer(mcp::Server& server, FakeHost& host)
    {
        for (uint16_t port = 18777; port < 18877; ++port)
        {
            mcp::ServerSettings settings;
            settings.port = port;
            settings.token = Token;
            settings.accessMode = mcp::AccessMode::AllowMutations;
            settings.subscriptionKeepAliveMillisecondsForTests = 50;
            if (server.Start(settings, &host)) return port;
        }
        throw std::runtime_error("failed to find a free MCP test port");
    }

    void TestModern(mcp::Server& server, uint16_t port)
    {
        const std::string discoverBody = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{" + Meta() + "}}";
        const std::string discover = Exchange(port, RequestText(port, "POST", discoverBody, "2026-07-28", "server/discover"));
        Require(discover.find("HTTP/1.1 200 OK") != std::string::npos, "modern discover failed");
        Require(discover.find("\"resultType\":\"complete\"") != std::string::npos, "discover resultType missing");
        Require(discover.find("\"supportedVersions\":[\"2026-07-28\",\"2025-11-25\",\"2025-06-18\"]") != std::string::npos, "supported versions missing");
        Require(discover.find("\"io.modelcontextprotocol/serverInfo\"") != std::string::npos, "modern serverInfo missing");
        Require(discover.find("MCP-Session-Id") == std::string::npos, "modern discover created a session");

        const std::string listBody = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{" + Meta() + "}}";
        const std::string list = Exchange(port, RequestText(port, "POST", listBody, "2026-07-28", "tools/list"));
        Require(list.find("\"ttlMs\":3600000") != std::string::npos && list.find("\"cacheScope\":\"private\"") != std::string::npos, "list cache metadata missing");
        Require(list.find("\"annotations\":{\"readOnlyHint\":true}") != std::string::npos, "read-only annotation missing");
        Require(list.find("lookdevpt.get_stats") < list.find("lookdevpt.get_state"), "tool order is not deterministic");
        Require(list.find("gltfExtensions") != std::string::npos &&
            list.find("clearcoatNormal") != std::string::npos &&
            list.find("resolutionPolicy") != std::string::npos &&
            list.find("uvSet") != std::string::npos,
            "glTF material mutation schema is missing");
        Require(list.find("lookdevpt.audit_scene") != std::string::npos &&
            list.find("lookdevpt.probe_surfaces") != std::string::npos &&
            list.find("lookdevpt.compare_captures") != std::string::npos &&
            list.find("lookdevpt.start_review") != std::string::npos &&
            list.find("lookdevpt.create_checkpoint") != std::string::npos &&
            list.find("lookdevpt.restore_checkpoint") != std::string::npos &&
            list.find("lookdevpt.start_benchmark") != std::string::npos &&
            list.find("lookdevpt.get_benchmark") != std::string::npos &&
            list.find("lookdevpt.cancel_benchmark") != std::string::npos,
            "automatic review tools are missing");

        const std::string resourcesBody = "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"resources/list\",\"params\":{" + Meta() + "}}";
        const std::string resources = Exchange(port, RequestText(port, "POST", resourcesBody, "2026-07-28", "resources/list"));
        Require(resources.find("lookdevpt://state") != std::string::npos && resources.find("\"ttlMs\":3600000") != std::string::npos, "modern resources/list failed");
        Require(resources.find("lookdevpt://scene/audit") != std::string::npos &&
            resources.find("lookdevpt://reviews/index") != std::string::npos &&
            resources.find("lookdevpt://checkpoints/index") != std::string::npos &&
            resources.find("lookdevpt://benchmarks/index") != std::string::npos,
            "automatic review resources are missing");

        const std::string templatesBody = "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"resources/templates/list\",\"params\":{" + Meta() + "}}";
        const std::string templates = Exchange(port, RequestText(port, "POST", templatesBody, "2026-07-28", "resources/templates/list"));
        Require(templates.find("lookdevpt://reviews/{id}") != std::string::npos &&
            templates.find("lookdevpt://comparisons/{id}") != std::string::npos &&
            templates.find("lookdevpt://comparisons/{id}/heatmap.png") != std::string::npos &&
            templates.find("lookdevpt://checkpoints/{id}") != std::string::npos &&
            templates.find("lookdevpt://benchmarks/{id}") != std::string::npos &&
            templates.find("lookdevpt://benchmarks/{id}/{artifact}") != std::string::npos,
            "automatic review resource templates are missing");

        const std::string promptsBody = "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"prompts/list\",\"params\":{" + Meta() + "}}";
        const std::string prompts = Exchange(port, RequestText(port, "POST", promptsBody, "2026-07-28", "prompts/list"));
        Require(prompts.find("lookdevpt.inspect_scene") != std::string::npos &&
            prompts.find("lookdevpt.review_scene") != std::string::npos &&
            prompts.find("lookdevpt.review_change") != std::string::npos,
            "modern prompts/list failed");

        const std::string promptBody = "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"prompts/get\",\"params\":{" + Meta() + ",\"name\":\"lookdevpt.inspect_scene\"}}";
        const std::string prompt = Exchange(port, RequestText(port, "POST", promptBody, "2026-07-28", "prompts/get", "lookdevpt.inspect_scene"));
        Require(prompt.find("\"resultType\":\"complete\"") != std::string::npos && prompt.find("Inspect the current D3D12LookDevPT session") != std::string::npos, "modern prompts/get failed");

        const std::string readBody = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\",\"params\":{" + Meta() + ",\"uri\":\"lookdevpt://state\"}}";
        const std::string read = Exchange(port, RequestText(port, "POST", readBody, "2026-07-28", "resources/read", "lookdevpt://state"));
        Require(read.find("\"ttlMs\":33") != std::string::npos, "resource TTL missing");

        const std::string encodedRead = Exchange(port, RequestText(port, "POST", readBody, "2026-07-28", "resources/read", "=?base64?bG9va2RldnB0Oi8vc3RhdGU=?="));
        Require(encodedRead.find("HTTP/1.1 200 OK") != std::string::npos, "Base64 sentinel Mcp-Name was rejected");

        const std::string callBody = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{" + Meta() + ",\"name\":\"lookdevpt.get_state\",\"arguments\":{}}}";
        const std::string call = Exchange(port, RequestText(port, "POST", callBody, "2026-07-28", "tools/call", "lookdevpt.get_state"));
        Require(call.find("\"structuredContent\":{\"ok\":true") != std::string::npos, "modern tool call failed");

        const cld::JsonValue emptyArguments =
            cld::JsonParser("{}").Parse();
        const std::string modernGrant =
            mcp::ApprovalGrantBroker::Instance().Issue(
                "modern-session-rejected",
                "lookdevpt.get_state",
                mcp::CanonicalArgumentsSha256(emptyArguments));
        const std::string modernGrantBody =
            "{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"tools/call\",\"params\":{" +
            Meta(modernGrant) +
            ",\"name\":\"lookdevpt.get_state\",\"arguments\":{}}}";
        const std::string modernGrantResponse = Exchange(port, RequestText(
            port, "POST", modernGrantBody, "2026-07-28", "tools/call",
            "lookdevpt.get_state", "modern-session-rejected"));
        Require(
            modernGrantResponse.find("invalid_approval_grant") !=
                std::string::npos,
            "modern MCP accepted an internal one-time approval token");

        const std::string chunkedCall = Exchange(port, ChunkedRequestText(port, callBody, "2026-07-28", "tools/call", "lookdevpt.get_state"));
        Require(chunkedCall.find("\"structuredContent\":{\"ok\":true") != std::string::npos, "chunked modern tool call failed");

        const std::string missingBody = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"resources/read\",\"params\":{" + Meta() + ",\"uri\":\"lookdevpt://missing\"}}";
        const std::string missing = Exchange(port, RequestText(port, "POST", missingBody, "2026-07-28", "resources/read", "lookdevpt://missing"));
        Require(missing.find("\"code\":-32602") != std::string::npos, "modern missing resource error is wrong");
        Require(missing.find("\"data\":{\"uri\":\"lookdevpt://missing\"}") != std::string::npos, "modern missing resource error omitted the URI");

        const std::string noMetaBody = "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/list\",\"params\":{}}";
        const std::string noMeta = Exchange(port, RequestText(port, "POST", noMetaBody, "2026-07-28", "tools/list"));
        Require(noMeta.find("HTTP/1.1 400 Bad Request") != std::string::npos && noMeta.find("\"code\":-32602") != std::string::npos, "missing metadata was accepted");

        const std::string mismatch = Exchange(port, RequestText(port, "POST", listBody, "2026-07-28", "resources/list"));
        Require(mismatch.find("\"code\":-32020") != std::string::npos, "header mismatch was accepted");

        const std::string cursorBody = "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"tools/list\",\"params\":{" + Meta() + ",\"cursor\":\"not-issued\"}}";
        const std::string cursor = Exchange(port, RequestText(port, "POST", cursorBody, "2026-07-28", "tools/list"));
        Require(cursor.find("\"code\":-32602") != std::string::npos, "unissued modern cursor was accepted");

        const std::string unknownVersion = Exchange(port, RequestText(port, "POST", listBody, "2099-01-01", "tools/list"));
        Require(unknownVersion.find("\"code\":-32022") != std::string::npos, "unknown version error is wrong");

        const std::string unknownBodyVersionRequest = "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"tools/list\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2099-01-01\",\"io.modelcontextprotocol/clientCapabilities\":{}}}}";
        const std::string unknownBodyVersionResponse = Exchange(port, RequestText(port, "POST", unknownBodyVersionRequest, {}, "tools/list"));
        Require(unknownBodyVersionResponse.find("\"code\":-32022") != std::string::npos, "unknown metadata version fell through to legacy routing");

        const std::string unknownBody = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"unknown/method\",\"params\":{" + Meta() + "}}";
        const std::string unknown = Exchange(port, RequestText(port, "POST", unknownBody, "2026-07-28", "unknown/method"));
        Require(unknown.find("HTTP/1.1 404 Not Found") != std::string::npos && unknown.find("\"code\":-32601") != std::string::npos, "unknown modern method status is wrong");

        const std::string modernDelete = Exchange(port, RequestText(port, "DELETE", {}, "2026-07-28"));
        Require(modernDelete.find("HTTP/1.1 405 Method Not Allowed") != std::string::npos, "modern DELETE was accepted");
        const std::string modernGet = Exchange(port, RequestText(port, "GET", {}, "2026-07-28"));
        Require(modernGet.find("HTTP/1.1 405 Method Not Allowed") != std::string::npos, "modern GET was accepted");

        const std::string nullOrigin = Exchange(port, RequestText(port, "POST", listBody, "2026-07-28", "tools/list", {}, {}, "null"));
        Require(nullOrigin.find("HTTP/1.1 403 Forbidden") != std::string::npos, "Origin null was accepted");
        const std::string loopbackOrigin = Exchange(port, RequestText(port, "POST", listBody, "2026-07-28", "tools/list", {}, {}, "http://localhost:3000"));
        Require(loopbackOrigin.find("HTTP/1.1 200 OK") != std::string::npos, "loopback Origin on a client port was rejected");
        const std::string remoteOrigin = Exchange(port, RequestText(port, "POST", listBody, "2026-07-28", "tools/list", {}, {}, "http://example.com"));
        Require(remoteOrigin.find("HTTP/1.1 403 Forbidden") != std::string::npos, "non-loopback Origin was accepted");
        (void)server;
    }

    void TestApprovalGrant(
        uint16_t port,
        FakeHost& host,
        const std::string& sessionId)
    {
        const cld::JsonValue canonicalFixture = cld::JsonParser(
            R"json({"b":1,"n":-0,"a":[true,null,"x"]})json").Parse();
        const std::string argumentsHash =
            mcp::CanonicalArgumentsSha256(canonicalFixture);
        Require(
            argumentsHash ==
                "fd580922504c7e68243ee62db404d9e22d08e1d5ee43548bee66890f06448ca9",
            "canonical argument hash fixture changed");

        constexpr const char* toolName = "lookdevpt.set_camera";
        const std::string grant = mcp::ApprovalGrantBroker::Instance().Issue(
            sessionId, toolName, argumentsHash);
        Require(grant.size() == 64, "one-time grant is not 256 bits");

        const std::string callBody =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":90,\"method\":\"tools/call\",\"params\":{") +
            "\"_meta\":{\"shaderjp.lookdevpt/approvalToken\":\"" +
            grant + "\"},\"name\":\"" + toolName +
            "\",\"arguments\":{\"a\":[true,null,\"x\"],\"n\":0,\"b\":1}}}";
        const std::string allowed = Exchange(port, RequestText(
            port, "POST", callBody, "2025-11-25", {}, {}, sessionId));
        Require(
            allowed.find("invalid_approval_grant") == std::string::npos &&
                host.lastOneTimeAuthorization.exchange(false),
            "matching one-time grant did not authorize the exact call");

        const std::string replayed = Exchange(port, RequestText(
            port, "POST", callBody, "2025-11-25", {}, {}, sessionId));
        Require(
            replayed.find("invalid_approval_grant") != std::string::npos &&
                !host.lastOneTimeAuthorization.exchange(false),
            "one-time grant was replayed");

        const std::string mismatchedGrant =
            mcp::ApprovalGrantBroker::Instance().Issue(
                sessionId, toolName, argumentsHash);
        const std::string mismatchedBody =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\",\"params\":{") +
            "\"_meta\":{\"shaderjp.lookdevpt/approvalToken\":\"" +
            mismatchedGrant + "\"},\"name\":\"" + toolName +
            "\",\"arguments\":{\"b\":2}}}";
        const std::string mismatched = Exchange(port, RequestText(
            port, "POST", mismatchedBody, "2025-11-25", {}, {}, sessionId));
        Require(
            mismatched.find("invalid_approval_grant") != std::string::npos,
            "grant accepted mismatched arguments");

        const std::string burned = Exchange(port, RequestText(
            port, "POST", callBody.substr(0, callBody.find(grant)) +
                mismatchedGrant + callBody.substr(callBody.find(grant) + grant.size()),
            "2025-11-25", {}, {}, sessionId));
        Require(
            burned.find("invalid_approval_grant") != std::string::npos,
            "mismatched grant was not consumed");

        const cld::JsonValue emptyArguments =
            cld::JsonParser("{}").Parse();
        const std::string secondInitializeBody =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"initialize\",") +
            "\"params\":{\"protocolVersion\":\"2025-11-25\"," +
            "\"capabilities\":{},\"clientInfo\":{\"name\":\"binding-test\"," +
            "\"version\":\"1\"}}}";
        const std::string secondInitialized = Exchange(port, RequestText(
            port, "POST", secondInitializeBody, "2025-11-25"));
        const std::string secondSessionId = HeaderValueFromResponse(
            secondInitialized, "MCP-Session-Id");
        Require(
            !secondSessionId.empty(),
            "binding test could not create a second legacy session");
        const std::string secondNotification =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        (void)Exchange(port, RequestText(
            port, "POST", secondNotification, "2025-11-25",
            {}, {}, secondSessionId));

        const std::string sessionBoundGrant =
            mcp::ApprovalGrantBroker::Instance().Issue(
                sessionId,
                "lookdevpt.get_state",
                mcp::CanonicalArgumentsSha256(emptyArguments));
        const std::string sessionBoundBody =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\",\"params\":{") +
            "\"_meta\":{\"shaderjp.lookdevpt/approvalToken\":\"" +
            sessionBoundGrant + "\"},\"name\":\"lookdevpt.get_state\"," +
            "\"arguments\":{}}}";
        const std::string wrongSession = Exchange(port, RequestText(
            port, "POST", sessionBoundBody, "2025-11-25",
            {}, {}, secondSessionId));
        const std::string burnedByWrongSession = Exchange(port, RequestText(
            port, "POST", sessionBoundBody, "2025-11-25",
            {}, {}, sessionId));
        Require(
            wrongSession.find("invalid_approval_grant") != std::string::npos &&
                burnedByWrongSession.find("invalid_approval_grant") !=
                    std::string::npos,
            "wrong-session presentation did not burn the approval grant");

        const std::string toolBoundGrant =
            mcp::ApprovalGrantBroker::Instance().Issue(
                sessionId,
                "lookdevpt.get_state",
                mcp::CanonicalArgumentsSha256(emptyArguments));
        const std::string wrongToolBody =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":95,\"method\":\"tools/call\",\"params\":{") +
            "\"_meta\":{\"shaderjp.lookdevpt/approvalToken\":\"" +
            toolBoundGrant + "\"},\"name\":\"lookdevpt.get_stats\"," +
            "\"arguments\":{}}}";
        const std::string expectedToolBody =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":96,\"method\":\"tools/call\",\"params\":{") +
            "\"_meta\":{\"shaderjp.lookdevpt/approvalToken\":\"" +
            toolBoundGrant + "\"},\"name\":\"lookdevpt.get_state\"," +
            "\"arguments\":{}}}";
        const std::string wrongTool = Exchange(port, RequestText(
            port, "POST", wrongToolBody, "2025-11-25",
            {}, {}, sessionId));
        const std::string burnedByWrongTool = Exchange(port, RequestText(
            port, "POST", expectedToolBody, "2025-11-25",
            {}, {}, sessionId));
        Require(
            wrongTool.find("invalid_approval_grant") != std::string::npos &&
                burnedByWrongTool.find("invalid_approval_grant") !=
                    std::string::npos,
            "wrong-tool presentation did not burn the approval grant");

        (void)Exchange(port, RequestText(
            port, "DELETE", {}, "2025-11-25",
            {}, {}, secondSessionId));

        const std::string japanese = Utf8Bytes({
            0xe6, 0x97, 0xa5,
            0xe6, 0x9c, 0xac,
            0xe8, 0xaa, 0x9e });
        const std::string emoji = Utf8Bytes({ 0xf0, 0x9f, 0x98, 0x80 });
        const cld::JsonValue unicodeArguments = cld::JsonParser(
            "{\"label\":\"" + japanese + emoji + "\"}").Parse();
        const std::string unicodeGrant =
            mcp::ApprovalGrantBroker::Instance().Issue(
                sessionId,
                toolName,
                mcp::CanonicalArgumentsSha256(unicodeArguments));
        const std::string unicodeWireBody =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\",\"params\":{") +
            "\"_meta\":{\"shaderjp.lookdevpt/approvalToken\":\"" +
            unicodeGrant + "\"},\"name\":\"" + toolName +
            "\",\"arguments\":{\"label\":\"" +
            "\\u65e5\\u672c\\u8a9e\\ud83d\\ude00\"}}}";
        const std::string unicodeWireResponse = Exchange(port, RequestText(
            port, "POST", unicodeWireBody, "2025-11-25",
            {}, {}, sessionId));
        Require(
            unicodeWireResponse.find("invalid_approval_grant") ==
                    std::string::npos &&
                host.lastOneTimeAuthorization.exchange(false),
            "escaped Japanese and emoji arguments did not match the native grant hash");
    }

    void TestLegacy(uint16_t port, FakeHost& host)
    {
        const std::string initializeBody = "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}";
        const std::string initialized = Exchange(port, RequestText(port, "POST", initializeBody, "2025-11-25"));
        const std::string sessionId = HeaderValueFromResponse(initialized, "MCP-Session-Id");
        Require(!sessionId.empty(), "legacy initialize did not create a session");
        Require(initialized.find("\"protocolVersion\":\"2025-11-25\"") != std::string::npos, "legacy version negotiation failed");
        Require(initialized.find("\"contractVersion\":\"1.0\"") != std::string::npos &&
            initialized.find("\"gltfMaterialExtensionsV1\":true") != std::string::npos &&
            initialized.find("\"textureResidencyV1\":true") != std::string::npos,
            "glTF compatibility features are missing from initialize");

        const cld::JsonValue emptyArguments =
            cld::JsonParser("{}").Parse();
        const std::string preInitializeGrant =
            mcp::ApprovalGrantBroker::Instance().Issue(
                sessionId,
                "lookdevpt.get_state",
                mcp::CanonicalArgumentsSha256(emptyArguments));
        const std::string preInitializeCallBody =
            std::string("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",") +
            "\"params\":{\"_meta\":{\"shaderjp.lookdevpt/approvalToken\":\"" +
            preInitializeGrant + "\"},\"name\":\"lookdevpt.get_state\"," +
            "\"arguments\":{}}}";
        const std::string preInitializeCall = Exchange(port, RequestText(
            port, "POST", preInitializeCallBody, "2025-11-25",
            {}, {}, sessionId));
        Require(
            preInitializeCall.find("invalid_approval_grant") != std::string::npos &&
                !host.lastOneTimeAuthorization.exchange(false),
            "grant was consumed before the legacy session was initialized");

        const std::string notificationBody = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        const std::string notification = Exchange(port, RequestText(port, "POST", notificationBody, "2025-11-25", {}, {}, sessionId));
        Require(notification.find("HTTP/1.1 202 Accepted") != std::string::npos, "legacy initialized notification failed");

        const std::string initializedCall = Exchange(port, RequestText(
            port, "POST", preInitializeCallBody, "2025-11-25",
            {}, {}, sessionId));
        Require(
            initializedCall.find("invalid_approval_grant") == std::string::npos &&
                host.lastOneTimeAuthorization.exchange(false),
            "grant was not accepted after legacy session initialization");

        TestApprovalGrant(port, host, sessionId);

        const std::string listBody = "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/list\",\"params\":{}}";
        const std::string list = Exchange(port, RequestText(port, "POST", listBody, "2025-11-25", {}, {}, sessionId));
        Require(list.find("HTTP/1.1 200 OK") != std::string::npos, "legacy tools/list failed");
        Require(list.find("\"resultType\"") == std::string::npos && list.find("\"cacheScope\"") == std::string::npos, "modern fields leaked into legacy response");

        const std::string callBody = "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{\"name\":\"lookdevpt.get_state\",\"arguments\":{}}}";
        const std::string call = Exchange(port, RequestText(port, "POST", callBody, "2025-11-25", {}, {}, sessionId));
        Require(call.find("\"structuredContent\":{\"ok\":true") != std::string::npos, "legacy tools/call failed");

        const std::string readBody = "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"resources/read\",\"params\":{\"uri\":\"lookdevpt://state\"}}";
        const std::string read = Exchange(port, RequestText(port, "POST", readBody, "2025-11-25", {}, {}, sessionId));
        Require(read.find("\"contents\":[{") != std::string::npos, "legacy resources/read failed");

        const std::string promptBody = "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"prompts/get\",\"params\":{\"name\":\"lookdevpt.inspect_scene\"}}";
        const std::string prompt = Exchange(port, RequestText(port, "POST", promptBody, "2025-11-25", {}, {}, sessionId));
        Require(prompt.find("Inspect the current D3D12LookDevPT session") != std::string::npos, "legacy prompts/get failed");

        const std::string deleted = Exchange(port, RequestText(port, "DELETE", {}, "2025-11-25", {}, {}, sessionId));
        Require(deleted.find("HTTP/1.1 202 Accepted") != std::string::npos, "legacy DELETE failed");
        const std::string afterDelete = Exchange(port, RequestText(port, "POST", listBody, "2025-11-25", {}, {}, sessionId));
        Require(afterDelete.find("HTTP/1.1 404 Not Found") != std::string::npos, "deleted session remained valid");
    }

    void TestPairing(mcp::Server& server, uint16_t port)
    {
        const std::string depthCode = server.BeginPairing();
        std::string wrongDepthCode = depthCode;
        wrongDepthCode[0] = wrongDepthCode[0] == '9'
            ? '0'
            : static_cast<char>(wrongDepthCode[0] + 1);
        const std::string boundaryBody =
            "{\"code\":\"" + wrongDepthCode +
            "\",\"clientName\":\"depth-boundary\",\"nested\":" +
            NestedArray(63) + "}";
        const std::string boundaryResponse = Exchange(
            port,
            PairingRequestText(port, "POST", "/pair", boundaryBody));
        Require(
            boundaryResponse.find("HTTP/1.1 401 Unauthorized") !=
                std::string::npos,
            "unauthenticated pairing rejected JSON at the depth boundary");

        const std::string excessiveBody =
            "{\"code\":\"" + wrongDepthCode +
            "\",\"clientName\":\"depth-excess\",\"nested\":" +
            NestedArray(64) + "}";
        const std::string excessiveResponse = Exchange(
            port,
            PairingRequestText(port, "POST", "/pair", excessiveBody));
        Require(
            excessiveResponse.find("HTTP/1.1 400 Bad Request") !=
                    std::string::npos &&
                excessiveResponse.find(
                    "JSON nesting depth exceeds the limit.") !=
                    std::string::npos,
            "unauthenticated pairing did not reject excessive JSON depth");

        const std::string code = server.BeginPairing();
        Require(code.size() == 8 && std::all_of(code.begin(), code.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }), "pairing code is not eight digits");
        const std::string discovery = Exchange(port, PairingRequestText(port, "GET", "/.well-known/lookdevpt/v1"));
        Require(discovery.find("HTTP/1.1 200 OK") != std::string::npos &&
            discovery.find("\"version\":\"0.2.0-beta.1\"") != std::string::npos &&
            discovery.find("\"contractVersion\":\"1.0\"") != std::string::npos,
            "pairing discovery identity failed");
        Require(discovery.find(code) == std::string::npos, "pairing discovery disclosed the code");
        const std::string remote = Exchange(port, PairingRequestText(port, "POST", "/pair", "{\"code\":\"" + code + "\",\"clientName\":\"remote\"}", "https://example.com"));
        Require(remote.find("HTTP/1.1 403 Forbidden") != std::string::npos, "pairing accepted a remote Origin");

        std::string wrongCode = code;
        wrongCode[0] = wrongCode[0] == '9' ? '0' : static_cast<char>(wrongCode[0] + 1);
        for (int attempt = 1; attempt <= 5; ++attempt)
        {
            const std::string wrong = Exchange(port, PairingRequestText(port, "POST", "/pair", "{\"code\":\"" + wrongCode + "\",\"clientName\":\"brute-force\"}"));
            Require(wrong.find(attempt == 5 ? "HTTP/1.1 429 Too Many Requests" : "HTTP/1.1 401 Unauthorized") != std::string::npos,
                "pairing brute-force limit is incorrect");
        }

        const std::string activeCode = server.BeginPairing();
        const std::string paired = Exchange(port, PairingRequestText(port, "POST", "/pair", "{\"code\":\"" + activeCode + "\",\"clientName\":\"LocalMCPChatClient Tests\"}"));
        Require(paired.find("HTTP/1.1 200 OK") != std::string::npos, "valid pairing failed");
        const std::string clientId = JsonStringField(paired, "clientId");
        const std::string pairedToken = JsonStringField(paired, "token");
        Require(!clientId.empty() && pairedToken.size() == 64, "pairing did not issue a 256-bit token");
        const std::string reused = Exchange(port, PairingRequestText(port, "POST", "/pair", "{\"code\":\"" + activeCode + "\",\"clientName\":\"reuse\"}"));
        Require(reused.find("HTTP/1.1 409 Conflict") != std::string::npos, "one-time pairing code was reused");

        const std::string initializeBody = "{\"jsonrpc\":\"2.0\",\"id\":80,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"paired\",\"version\":\"1\"}}}";
        const std::string authorized = Exchange(port, RequestText(port, "POST", initializeBody, "2025-11-25", {}, {}, {}, {}, pairedToken));
        Require(authorized.find("HTTP/1.1 200 OK") != std::string::npos, "paired token was not authorized");
        Require(server.RevokePairedClient(clientId), "paired client could not be revoked");
        const std::string revoked = Exchange(port, RequestText(port, "POST", initializeBody, "2025-11-25", {}, {}, {}, {}, pairedToken));
        Require(revoked.find("HTTP/1.1 401 Unauthorized") != std::string::npos, "revoked token remained authorized");
    }

    void TestSubscription(mcp::Server& server, uint16_t port)
    {
        const std::string body = "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"subscriptions/listen\",\"params\":{" + Meta() +
            ",\"notifications\":{\"toolsListChanged\":true,\"resourceSubscriptions\":[\"lookdevpt://state\",\"lookdevpt://reviews/index\",\"lookdevpt://unsupported\",\"lookdevpt://state\"]}}}";
        SOCKET socketValue = Connect(port);
        SendAll(socketValue, RequestText(port, "POST", body, "2026-07-28", "subscriptions/listen"));
        const std::string acknowledged = ReceiveUntil(socketValue, "notifications/subscriptions/acknowledged");
        Require(acknowledged.find("Content-Type: text/event-stream") != std::string::npos, "subscription did not use SSE");
        Require(acknowledged.find("data: ") < acknowledged.find("notifications/subscriptions/acknowledged"), "subscription acknowledgment was not the first SSE event");
        Require(acknowledged.find("lookdevpt://state") != std::string::npos &&
            acknowledged.find("lookdevpt://reviews/index") != std::string::npos &&
            acknowledged.find("lookdevpt://unsupported") == std::string::npos,
            "subscription filter was not acknowledged correctly");
        Require(acknowledged.find("toolsListChanged") == std::string::npos, "unsupported list notification was acknowledged");

        const std::string secondBody = "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"subscriptions/listen\",\"params\":{" + Meta() +
            ",\"notifications\":{\"resourceSubscriptions\":[\"lookdevpt://stats\"]}}}";
        SOCKET secondSocket = Connect(port);
        SendAll(secondSocket, RequestText(port, "POST", secondBody, "2026-07-28", "subscriptions/listen"));
        const std::string secondAcknowledged = ReceiveUntil(secondSocket, "notifications/subscriptions/acknowledged");
        Require(secondAcknowledged.find("lookdevpt://stats") != std::string::npos, "second subscription was not acknowledged");

        server.PublishResourceUpdates({ "lookdevpt://stats", "lookdevpt://state", "lookdevpt://state" });
        const std::string updated = ReceiveUntil(socketValue, "notifications/resources/updated");
        Require(updated.find("\"uri\":\"lookdevpt://state\"") != std::string::npos, "subscribed resource update missing");
        Require(updated.find("lookdevpt://stats") == std::string::npos, "unsubscribed resource update leaked");
        Require(updated.find("notifications/resources/updated") == updated.rfind("notifications/resources/updated"), "duplicate revision notification was not coalesced");
        const std::string secondUpdated = ReceiveUntil(secondSocket, "notifications/resources/updated");
        Require(secondUpdated.find("\"uri\":\"lookdevpt://stats\"") != std::string::npos && secondUpdated.find("lookdevpt://state") == std::string::npos, "multiple subscription filters crossed streams");

        server.PublishResourceUpdates({ "lookdevpt://reviews/index" });
        const std::string reviewUpdated = ReceiveUntil(socketValue, "notifications/resources/updated");
        Require(reviewUpdated.find("\"uri\":\"lookdevpt://reviews/index\"") != std::string::npos, "review progress resource update missing");

        const std::string keepAlive = ReceiveUntil(socketValue, ":\r\n\r\n");
        Require(keepAlive.find(":\r\n\r\n") != std::string::npos, "subscription keepalive was not sent");

        shutdown(secondSocket, SD_BOTH);
        closesocket(secondSocket);
        server.PublishResourceUpdates({ "lookdevpt://stats" });
        for (int attempt = 0; attempt < 50 && server.GetStatus().activeSubscriptions > 1; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Require(server.GetStatus().activeSubscriptions == 1, "disconnected client subscription was retained");
        server.Stop();
        const std::string completed = ReceiveUntil(socketValue, "\"resultType\":\"complete\"");
        Require(completed.find("\"io.modelcontextprotocol/subscriptionId\":20") != std::string::npos, "subscription did not close gracefully");
        closesocket(socketValue);
    }
}

int main(int argc, char** argv)
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::cerr << "McpServerTests: WSAStartup failed\n";
        return 1;
    }

    mcp::Server server;
    FakeHost host;
    try
    {
        if ((argc == 3 && std::string(argv[1]) == "--serve") ||
            (argc == 4 && std::string(argv[1]) == "--serve-auth"))
        {
            const int requestedPort = std::atoi(argv[2]);
            Require(requestedPort > 0 && requestedPort <= 65535, "invalid conformance host port");
            mcp::ServerSettings settings;
            settings.port = static_cast<uint16_t>(requestedPort);
            settings.allowUnauthenticatedLoopbackForTests = argc == 3;
            if (argc == 4) settings.token = argv[3];
            Require(server.Start(settings, &host), "failed to start conformance host");
            std::cout << "MCP conformance host listening on " << settings.port << "\n";
            std::cout.flush();
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(60));
            }
        }
        TestJsonDepthLimit();
        TestJsonUnicodeStrings();
        TestJsonNumberGrammarAndLocale();
        TestCanonicalArgumentFixtures();
        TestToolCatalogReadOnlyAnnotations();
        const uint16_t port = StartServer(server, host);
        TestModern(server, port);
        TestLegacy(port, host);
        TestPairing(server, port);
        TestSubscription(server, port);
        server.Stop();
        WSACleanup();
        std::cout << "McpServerTests passed\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        server.Stop();
        WSACleanup();
        std::cerr << "McpServerTests failed: " << ex.what() << "\n";
        return 1;
    }
}

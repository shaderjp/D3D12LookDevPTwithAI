#include "McpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
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
        mcp::ToolResult CallMcpTool(const std::string& name, const cld::JsonValue&, int) override
        {
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
    };

    void Require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::string Meta()
    {
        return "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
            "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"mcp-server-tests\",\"version\":\"1.0\"},"
            "\"io.modelcontextprotocol/clientCapabilities\":{}}";
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

    void TestLegacy(uint16_t port)
    {
        const std::string initializeBody = "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}";
        const std::string initialized = Exchange(port, RequestText(port, "POST", initializeBody, "2025-11-25"));
        const std::string sessionId = HeaderValueFromResponse(initialized, "MCP-Session-Id");
        Require(!sessionId.empty(), "legacy initialize did not create a session");
        Require(initialized.find("\"protocolVersion\":\"2025-11-25\"") != std::string::npos, "legacy version negotiation failed");

        const std::string notificationBody = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        const std::string notification = Exchange(port, RequestText(port, "POST", notificationBody, "2025-11-25", {}, {}, sessionId));
        Require(notification.find("HTTP/1.1 202 Accepted") != std::string::npos, "legacy initialized notification failed");

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
        const std::string code = server.BeginPairing();
        Require(code.size() == 8 && std::all_of(code.begin(), code.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }), "pairing code is not eight digits");
        const std::string discovery = Exchange(port, PairingRequestText(port, "GET", "/.well-known/lookdevpt/v1"));
        Require(discovery.find("HTTP/1.1 200 OK") != std::string::npos && discovery.find("\"contractVersion\":\"1.0\"") != std::string::npos, "pairing discovery failed");
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
        const uint16_t port = StartServer(server, host);
        TestModern(server, port);
        TestLegacy(port);
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

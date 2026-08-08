#include "McpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct HttpResponse
{
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string Trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

bool SendAll(SOCKET socket, const std::string& bytes)
{
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const int sent = send(socket, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
        if (sent <= 0)
        {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

HttpResponse Exchange(uint16_t port, const std::vector<std::string>& parts)
{
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(client != INVALID_SOCKET, "client socket creation failed");
    DWORD timeoutMs = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    Require(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR,
        "client connection failed");

    for (const std::string& part : parts)
    {
        Require(SendAll(client, part), "client request send failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    shutdown(client, SD_SEND);

    std::string text;
    char buffer[8192];
    for (;;)
    {
        const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received == 0)
        {
            break;
        }
        Require(received > 0, "client response read failed");
        text.append(buffer, static_cast<size_t>(received));
    }
    closesocket(client);

    const size_t headerEnd = text.find("\r\n\r\n");
    Require(headerEnd != std::string::npos, "HTTP response headers were incomplete");
    std::istringstream headers(text.substr(0, headerEnd));
    std::string statusLine;
    std::getline(headers, statusLine);
    std::istringstream status(statusLine);
    std::string version;
    HttpResponse response;
    status >> version >> response.status;
    Require(response.status != 0, "HTTP response status was invalid");

    std::string line;
    while (std::getline(headers, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
            response.headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
        }
    }
    response.body = text.substr(headerEnd + 4);
    return response;
}

uint16_t FindAvailablePort()
{
    WSADATA data{};
    Require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed while selecting a port");
    SOCKET candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(candidate != INVALID_SOCKET, "port-selection socket creation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    Require(bind(candidate, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR,
        "port-selection bind failed");
    int addressBytes = sizeof(address);
    Require(getsockname(candidate, reinterpret_cast<sockaddr*>(&address), &addressBytes) != SOCKET_ERROR,
        "port-selection getsockname failed");
    const uint16_t port = ntohs(address.sin_port);
    closesocket(candidate);
    WSACleanup();
    return port;
}

std::string AuthorizationHeaders(const std::string& token)
{
    std::string headers;
    if (!token.empty())
    {
        headers = "Authorization: Bearer " + token + "\r\n";
    }
    return headers +
        "Accept: application/json, text/event-stream\r\n"
        "Content-Type: application/json\r\n";
}

std::vector<std::string> ContentLengthRequest(
    const std::string& method,
    const std::string& token,
    const std::string& body,
    const std::string& additionalHeaders = {})
{
    const std::string headers = method + " /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
        AuthorizationHeaders(token) + additionalHeaders +
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    const size_t midpoint = body.size() / 2;
    return { headers, body.substr(0, midpoint), body.substr(midpoint) };
}

std::vector<std::string> ChunkedRequest(
    const std::string& token,
    const std::string& body,
    const std::vector<size_t>& chunkSizes,
    const std::string& additionalHeaders = {},
    const std::string& extension = {},
    const std::vector<std::string>& trailers = {})
{
    std::vector<std::string> parts;
    parts.push_back("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n" + AuthorizationHeaders(token) +
        additionalHeaders + "Transfer-Encoding: chunked\r\n\r\n");
    size_t offset = 0;
    size_t chunkIndex = 0;
    while (offset < body.size())
    {
        const size_t requested = chunkSizes.empty() ? body.size() : chunkSizes[chunkIndex % chunkSizes.size()];
        const size_t size = (std::min)(requested, body.size() - offset);
        std::ostringstream sizeLine;
        sizeLine << std::hex << size;
        if (!extension.empty()) sizeLine << ";" << extension;
        sizeLine << "\r\n";
        parts.push_back(sizeLine.str());
        parts.push_back(body.substr(offset, size));
        parts.push_back("\r\n");
        offset += size;
        ++chunkIndex;
    }
    parts.push_back("0\r\n");
    for (const std::string& trailer : trailers)
    {
        parts.push_back(trailer + "\r\n");
    }
    parts.push_back("\r\n");
    return parts;
}

std::string Header(const HttpResponse& response, const std::string& name)
{
    const auto found = response.headers.find(Lower(name));
    return found == response.headers.end() ? std::string{} : found->second;
}

size_t CountOccurrences(const std::string& text, const std::string& pattern)
{
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(pattern, offset)) != std::string::npos)
    {
        ++count;
        offset += pattern.size();
    }
    return count;
}

class TestHost final : public mcp::IServerHost
{
public:
    mcp::ToolResult CallMcpTool(const std::string& name, const cld::JsonValue&, int) override
    {
        ++toolCalls;
        correctTool.store(name == "lookdevpt.get_stats");
        mcp::ToolResult result;
        result.ok = true;
        result.text = "stats available";
        result.structuredJson = R"json({"available":true})json";
        return result;
    }

    mcp::ResourceResult ReadMcpResource(const std::string&) override
    {
        ++resourceCalls;
        return {};
    }

    size_t PendingMcpCommandCount() const override
    {
        return 0;
    }

    std::atomic_size_t toolCalls = 0;
    std::atomic_size_t resourceCalls = 0;
    std::atomic_bool correctTool = true;
};
}

int main()
{
    Require(
        mcp::AuthenticationModeName(
            mcp::AuthenticationMode::BearerToken) == "bearer_token",
        "Bearer authentication mode name changed unexpectedly");
    Require(
        mcp::AuthenticationModeFromName(
            "none",
            mcp::AuthenticationMode::BearerToken) ==
            mcp::AuthenticationMode::None,
        "None authentication mode was not parsed");

    const uint16_t port = FindAvailablePort();
    const std::string token = mcp::GenerateToken();
    TestHost host;
    mcp::Server server;
    mcp::ServerSettings settings;
    settings.port = port;
    settings.token = token;
    settings.accessMode = mcp::AccessMode::ReadOnly;
    Require(server.Start(settings, &host), "MCP test server failed to start");

    const std::string initialize =
        R"json({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"http-test","version":"1"}}})json";
    const std::string protocolHeader = "MCP-Protocol-Version: 2025-11-25\r\n";

    HttpResponse fixedInitialize = Exchange(port, ContentLengthRequest("POST", token, initialize));
    Require(fixedInitialize.status == 200, "Content-Length initialize did not return 200");
    const std::string fixedSession = Header(fixedInitialize, "MCP-Session-Id");
    Require(!fixedSession.empty(), "Content-Length initialize did not return a session id");
    HttpResponse fixedDelete = Exchange(port, ContentLengthRequest(
        "DELETE", token, {}, protocolHeader + "MCP-Session-Id: " + fixedSession + "\r\n"));
    Require(fixedDelete.status == 202, "Content-Length session DELETE did not return 202");

    HttpResponse chunkedInitialize = Exchange(port, ChunkedRequest(token, initialize, { 1 }));
    Require(chunkedInitialize.status == 200, "one-byte chunked initialize did not return 200");
    const std::string session = Header(chunkedInitialize, "MCP-Session-Id");
    Require(!session.empty(), "chunked initialize did not return a session id");
    const std::string sessionHeaders = protocolHeader + "MCP-Session-Id: " + session + "\r\n";

    const std::string initialized = R"json({"jsonrpc":"2.0","method":"notifications/initialized"})json";
    HttpResponse initializedResponse = Exchange(
        port, ChunkedRequest(token, initialized, { 2, 7, 1, 9 }, sessionHeaders, "client=test"));
    Require(initializedResponse.status == 202 && initializedResponse.body.empty(),
        "chunked initialized notification did not return an empty 202 response");

    const std::string toolsList = R"json({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})json";
    HttpResponse toolsResponse = Exchange(port, ChunkedRequest(
        token, toolsList, { 3, 11, 2 }, sessionHeaders, {}, { "X-Test-Trailer: accepted" }));
    Require(toolsResponse.status == 200, "chunked tools/list did not return 200");
    Require(CountOccurrences(toolsResponse.body, "\"name\":\"lookdevpt.") == 36,
        "tools/list did not return all 36 tools");

    const std::string toolCall =
        R"json({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"lookdevpt.get_stats","arguments":{}}})json";
    HttpResponse toolResponse = Exchange(port, ChunkedRequest(token, toolCall, { 5, 13, 4 }, sessionHeaders));
    Require(toolResponse.status == 200 && toolResponse.body.find("\"isError\":true") == std::string::npos,
        "chunked read-only tools/call failed");
    Require(host.toolCalls.load() == 1 && host.correctTool.load(), "the expected read-only tool was not called");

    const size_t callsBeforeMalformed = host.toolCalls.load();
    const std::string base = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n" + AuthorizationHeaders(token);
    Require(Exchange(port, { base + "Transfer-Encoding: chunked\r\n\r\nZ\r\n" }).status == 400,
        "invalid hexadecimal chunk size was not rejected");
    Require(Exchange(port, { base + "Transfer-Encoding: chunked\r\n\r\nFFFFFFFFFFFFFFFFF\r\n" }).status == 400,
        "overflowing chunk size was not rejected");
    Require(Exchange(port, { base + "Transfer-Encoding: chunked\r\n\r\n3\r\nabcX" }).status == 400,
        "missing chunk-data CRLF was not rejected");
    Require(Exchange(port, { base + "Transfer-Encoding: chunked\r\n\r\n5\r\nabc" }).status == 400,
        "incomplete chunk body was not rejected");
    Require(Exchange(port, { base + "Transfer-Encoding: chunked\r\n\r\n1000001\r\n" }).status == 413,
        "decoded body above 16 MiB was not rejected with 413");
    Require(Exchange(port, { base + "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n" }).status == 400,
        "Content-Length plus Transfer-Encoding was not rejected");
    Require(Exchange(port, { base + "Transfer-Encoding: gzip\r\n\r\n" }).status == 400,
        "non-chunked final transfer coding was not rejected with 400");
    Require(Exchange(port, { base + "Transfer-Encoding: gzip, chunked\r\n\r\n" }).status == 501,
        "unsupported transfer coding was not rejected with 501");
    Require(host.toolCalls.load() == callsBeforeMalformed && host.resourceCalls.load() == 0,
        "malformed HTTP framing reached the MCP host");

    HttpResponse getResponse = Exchange(port, {
        "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n" + AuthorizationHeaders(token) + "Content-Length: 0\r\n\r\n" });
    Require(getResponse.status == 405 && Header(getResponse, "Allow") == "POST, DELETE",
        "GET /mcp did not preserve its 405 Allow response");
    Require(Exchange(port, { "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n" }).status == 401,
        "missing authentication did not return 401");
    Require(Exchange(port, { base + "Origin: https://example.invalid\r\nContent-Length: 0\r\n\r\n" }).status == 403,
        "invalid Origin did not return 403");

    HttpResponse deleteResponse = Exchange(port, ContentLengthRequest(
        "DELETE", token, {}, sessionHeaders));
    Require(deleteResponse.status == 202, "chunked-client session DELETE did not return 202");
    Require(server.GetStatus().activeLegacySessions == 0, "MCP session remained after DELETE");

    WSADATA stopData{};
    Require(WSAStartup(MAKEWORD(2, 2), &stopData) == 0, "WSAStartup failed for stop test");
    SOCKET incompleteClient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(incompleteClient != INVALID_SOCKET, "incomplete-request socket creation failed");
    sockaddr_in stopAddress{};
    stopAddress.sin_family = AF_INET;
    stopAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    stopAddress.sin_port = htons(port);
    Require(connect(incompleteClient, reinterpret_cast<sockaddr*>(&stopAddress), sizeof(stopAddress)) != SOCKET_ERROR,
        "incomplete-request connection failed");
    Require(SendAll(incompleteClient, "POST /mcp HTTP/1.1\r\n"), "incomplete request send failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto stopStarted = std::chrono::steady_clock::now();
    server.Stop();
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
    closesocket(incompleteClient);
    WSACleanup();
    Require(stopElapsed < std::chrono::seconds(2), "server stop blocked on an incomplete client request");

    Require(server.Start(settings, &host), "MCP test server failed to restart for idle-stop test");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto idleStopStarted = std::chrono::steady_clock::now();
    server.Stop();
    const auto idleStopElapsed = std::chrono::steady_clock::now() - idleStopStarted;
    Require(idleStopElapsed < std::chrono::seconds(2), "server stop blocked on an idle listening socket");

    settings.authenticationMode = mcp::AuthenticationMode::None;
    settings.token.clear();
    Require(server.Start(settings, &host),
        "MCP server rejected None authentication with an empty token");
    HttpResponse noAuthInitialize = Exchange(
        port,
        ContentLengthRequest("POST", {}, initialize));
    Require(noAuthInitialize.status == 200,
        "None authentication initialize did not return 200");
    const std::string noAuthSession =
        Header(noAuthInitialize, "MCP-Session-Id");
    Require(!noAuthSession.empty(),
        "None authentication initialize did not return a session id");
    const std::string noAuthSessionHeaders =
        protocolHeader + "MCP-Session-Id: " + noAuthSession + "\r\n";
    HttpResponse noAuthTools = Exchange(
        port,
        ContentLengthRequest(
            "POST", {}, toolsList, noAuthSessionHeaders));
    Require(noAuthTools.status == 200,
        "None authentication tools/list did not return 200");
    HttpResponse noAuthDelete = Exchange(
        port,
        ContentLengthRequest(
            "DELETE", {}, {}, noAuthSessionHeaders));
    Require(noAuthDelete.status == 202,
        "None authentication session DELETE did not return 202");
    server.Stop();
    return 0;
}

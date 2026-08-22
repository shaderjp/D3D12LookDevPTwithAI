#pragma once

#include "McpDispatcher.h"
#include "SimpleJson.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mcp
{
inline constexpr const char* ApplicationVersion = "0.2.0-beta.1";
inline constexpr const char* ContractVersion = "1.0";
inline constexpr const char* ProtocolVersion = "2026-07-28";

enum class AuthenticationMode
{
    BearerToken = 0,
    None = 1,
};

struct ServerSettings
{
    uint16_t port = 8777;
    int requestTimeoutSeconds = 120;
    AccessMode accessMode = AccessMode::ConfirmMutations;
    AuthenticationMode authenticationMode =
        AuthenticationMode::BearerToken;
    std::string token;
    bool allowUnauthenticatedLoopbackForTests = false;
    int subscriptionKeepAliveMillisecondsForTests = 0;
    int requestReceiveDeadlineMillisecondsForTests = 0;
    size_t httpBodyBudgetBytesForTests = 0;
    std::string pairedClientsPath;
};

struct ServerStatus
{
    bool running = false;
    uint16_t port = 0;
    std::string endpoint;
    std::string lastError;
    size_t activeLegacySessions = 0;
    size_t activeSubscriptions = 0;
    size_t activeRequests = 0;
    std::vector<std::string> recentRequests;
    std::string pairingCode;
    int pairingSecondsRemaining = 0;
    std::vector<std::pair<std::string, std::string>> pairedClients;
};

struct ToolResult
{
    bool ok = false;
    bool isError = false;
    std::string text;
    std::string structuredJson = "{}";
    std::string contentJson;
};

struct ToolCallAuthorization
{
    bool oneTimeMutationGrant = false;
};

// Process-local broker used only by the integrated Assistant UI and this MCP
// server. Grants are opaque, expire after 30 seconds, and are consumed on the
// first matching (or mismatching) presentation.
class ApprovalGrantBroker
{
public:
    static ApprovalGrantBroker& Instance();

    std::string Issue(
        const std::string& clientSession,
        const std::string& toolName,
        const std::string& canonicalArgumentsSha256);
    bool Consume(
        const std::string& grant,
        const std::string& clientSession,
        const std::string& toolName,
        const cld::JsonValue& arguments);
    void RevokeAll() noexcept;

private:
    struct Record
    {
        std::string clientSession;
        std::string toolName;
        std::string argumentsSha256;
        std::chrono::steady_clock::time_point expiresAt;
    };

    std::mutex m_mutex;
    std::unordered_map<std::string, Record> m_records;
};

std::string CanonicalArgumentsSha256(const cld::JsonValue& arguments);

struct ResourceResult
{
    bool ok = false;
    std::string uri;
    std::string mimeType = "application/json";
    std::string text;
    std::string blob;
    std::string error;
};

class IServerHost
{
public:
    virtual ~IServerHost() = default;
    virtual ToolResult CallMcpTool(
        const std::string& name,
        const cld::JsonValue& arguments,
        int timeoutMs,
        ToolCallAuthorization authorization) = 0;
    virtual ResourceResult ReadMcpResource(const std::string& uri) = 0;
    virtual size_t PendingMcpCommandCount() const = 0;
};

class Server
{
public:
    Server();
    ~Server();

    bool Start(const ServerSettings& settings, IServerHost* host);
    void Stop();
    bool IsRunning() const;
    ServerStatus GetStatus() const;
    void PublishResourceUpdates(const std::vector<std::string>& uris);
    std::string BeginPairing();
    bool RevokePairedClient(const std::string& clientId);
    void RevokeAllPairedClients();

    struct HttpRequest
    {
        std::string method;
        std::string target;
        std::string path;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    struct HttpResponse
    {
        int status = 200;
        std::string reason = "OK";
        std::string contentType = "application/json";
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        bool subscriptionStream = false;
        std::string subscriptionIdJson;
        std::vector<std::string> subscriptionUris;
    };

private:
    struct Session
    {
        std::string protocolVersion;
        bool initialized = false;
        std::chrono::steady_clock::time_point lastSeen;
    };

    struct Worker
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> complete;
        std::shared_ptr<std::atomic<bool>> socketClosed;
        std::shared_ptr<std::atomic<bool>> subscriptionStream;
        uintptr_t socketValue = 0;
    };

    struct PairedClient
    {
        std::string id;
        std::string name;
        std::string tokenHash;
    };

    void Run();
    void HandleClient(
        uintptr_t socketValue,
        const std::shared_ptr<std::atomic<bool>>& complete,
        const std::shared_ptr<std::atomic<bool>>& socketClosed,
        const std::shared_ptr<std::atomic<bool>>& subscriptionStream);
    void ReapWorkers();
    bool ReadHttpRequest(
        uintptr_t socketValue,
        HttpRequest& request,
        int& errorStatus,
        std::string& error,
        bool& preflightRejected,
        HttpResponse& preflightResponse,
        size_t& reservedBodyBytes);
    bool SendHttpResponse(uintptr_t socketValue, const HttpResponse& response) const;
    bool SendBytes(uintptr_t socketValue, const std::string& bytes) const;
    void StreamSubscription(uintptr_t socketValue, const HttpResponse& response);
    HttpResponse HandleHttpRequest(const HttpRequest& request);
    HttpResponse HandleJsonRpc(const HttpRequest& request, const cld::JsonValue& rpc, bool modern);
    HttpResponse HandlePairRequest(const HttpRequest& request);
    HttpResponse HandleInitialize(const cld::JsonValue& rpc);
    HttpResponse HandlePromptGet(const std::string& idJson, const cld::JsonValue& rpc, bool modern);
    HttpResponse HandleDeleteSession(const HttpRequest& request);
    bool PreflightHttpRequest(
        const HttpRequest& request,
        HttpResponse& response);
    bool TryReserveHttpBodyBytes(size_t byteCount);
    void ReleaseHttpBodyBytes(size_t byteCount) noexcept;
    bool ValidateOrigin(const HttpRequest& request) const;
    bool ValidateHost(const HttpRequest& request) const;
    bool ValidateAuthorization(const HttpRequest& request) const;
    bool ValidateProtocolHeader(const HttpRequest& request, std::string& diagnostics) const;
    bool ValidateModernRequest(const HttpRequest& request, const cld::JsonValue& rpc, int& errorCode, std::string& diagnostics) const;
    bool ResolveSession(const HttpRequest& request, Session& session, std::string& diagnostics);
    void MarkSessionInitialized(const HttpRequest& request);
    void PruneLegacySessionsLocked(std::chrono::steady_clock::time_point now);
    void AppendLog(const std::string& message);
    void LoadPairedClients();
    void SavePairedClientsLocked() const;

    ServerSettings m_settings;
    IServerHost* m_host = nullptr;
    std::atomic<bool> m_running = false;
    std::atomic<bool> m_stopRequested = false;
    std::atomic<size_t> m_activeRequests = 0;
    uintptr_t m_listenSocket = 0;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::vector<Worker> m_workers;
    std::map<std::string, Session> m_sessions;
    std::map<std::string, uint64_t> m_resourceGenerations;
    std::condition_variable m_subscriptionCv;
    std::atomic<size_t> m_activeConnections = 0;
    std::atomic<size_t> m_activeSubscriptions = 0;
    std::atomic<size_t> m_reservedHttpBodyBytes = 0;
    std::vector<std::string> m_recentRequests;
    std::string m_lastError;
    std::string m_pairingCode;
    std::chrono::steady_clock::time_point m_pairingExpiresAt{};
    int m_pairingFailures = 0;
    std::vector<PairedClient> m_pairedClients;
};

std::string GenerateToken(size_t byteCount = 24);
std::string AuthenticationModeName(AuthenticationMode mode);
AuthenticationMode AuthenticationModeFromName(
    const std::string& name,
    AuthenticationMode fallback);
std::string AccessModeName(AccessMode mode);
AccessMode AccessModeFromName(const std::string& name, AccessMode fallback);
std::string BuildActionsSchemaJson();
std::string BuildToolsListJson();
std::string BuildResourcesListJson();
std::string BuildResourceTemplatesListJson();
std::string BuildPromptsListJson();
std::string BuildPromptGetResultJson(const std::string& name, const cld::JsonValue* arguments, bool& found);
}

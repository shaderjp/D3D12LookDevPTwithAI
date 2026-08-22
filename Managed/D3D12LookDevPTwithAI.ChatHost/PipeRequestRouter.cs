using System.Text.Json;
using D3D12LookDevPTwithAI.Chat.Core;
using Microsoft.Extensions.Hosting;

namespace D3D12LookDevPTwithAI.ChatHost;

public sealed class PipeRequestRouter(
    ChatCoordinator coordinator,
    IHostApplicationLifetime applicationLifetime)
{
    private int _initialized;

    public async Task HandleAsync(
        PipeEnvelope request,
        IPipePeer peer,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(request);
        ArgumentNullException.ThrowIfNull(peer);
        if (request.Kind != PipeMessageKind.Request)
            throw new PipeProtocolException("ChatHost accepts request frames from the native host.");

        try
        {
            switch (request.Method)
            {
                case "initialize":
                    {
                        if (Interlocked.Exchange(ref _initialized, 1) != 0)
                            throw new ChatRequestException("already_initialized", "initialize may only be called once.");
                        InitializeResult result;
                        try
                        {
                            result = await coordinator.InitializeAsync(
                                Deserialize<InitializeRequest>(request),
                                cancellationToken).ConfigureAwait(false);
                        }
                        catch
                        {
                            Interlocked.Exchange(ref _initialized, 0);
                            throw;
                        }
                        await peer.SendResponseAsync(request, result, cancellationToken: cancellationToken).ConfigureAwait(false);
                        return;
                    }
                case "conversation.list":
                    RequireInitialized();
                    await peer.SendResponseAsync(
                        request,
                        await coordinator.ListConversationsAsync(cancellationToken).ConfigureAwait(false),
                        cancellationToken: cancellationToken).ConfigureAwait(false);
                    return;
                case "conversation.create":
                    RequireInitialized();
                    await peer.SendResponseAsync(
                        request,
                        await coordinator.CreateConversationAsync(
                            Deserialize<ConversationCreateRequest>(request),
                            cancellationToken).ConfigureAwait(false),
                        cancellationToken: cancellationToken).ConfigureAwait(false);
                    return;
                case "conversation.select":
                    RequireInitialized();
                    await peer.SendResponseAsync(
                        request,
                        await coordinator.SelectConversationAsync(
                            Deserialize<ConversationSelectRequest>(request),
                            cancellationToken).ConfigureAwait(false),
                        cancellationToken: cancellationToken).ConfigureAwait(false);
                    return;
                case "sendTurn":
                    {
                        RequireInitialized();
                        var turnRequest = Deserialize<SendTurnRequest>(request);
                        var prepared = await coordinator.PrepareTurnAsync(
                            turnRequest,
                            request.RequestId,
                            peer,
                            cancellationToken).ConfigureAwait(false);
                        try
                        {
                            await peer.SendResponseAsync(
                                request,
                                new SendTurnResult(turnRequest.TurnId, true),
                                cancellationToken: cancellationToken).ConfigureAwait(false);
                            prepared.Start();
                        }
                        catch
                        {
                            prepared.Abort();
                            throw;
                        }
                        return;
                    }
                case "cancelTurn":
                    RequireInitialized();
                    await peer.SendResponseAsync(
                        request,
                        coordinator.CancelTurn(Deserialize<CancelTurnRequest>(request)),
                        cancellationToken: cancellationToken).ConfigureAwait(false);
                    return;
                case "approval.respond":
                    RequireInitialized();
                    await peer.SendResponseAsync(
                        request,
                        coordinator.RespondToApproval(Deserialize<ApprovalRespondRequest>(request)),
                        cancellationToken: cancellationToken).ConfigureAwait(false);
                    return;
                case "shutdown":
                    RequireInitialized();
                    await peer.SendResponseAsync(request, new { accepted = true }, cancellationToken: cancellationToken).ConfigureAwait(false);
                    applicationLifetime.StopApplication();
                    return;
                default:
                    throw new ChatRequestException("method_not_found", $"Unknown ChatHost method: {request.Method}");
            }
        }
        catch (ChatRequestException exception)
        {
            await peer.SendResponseAsync(
                request,
                new { },
                new PipeError(exception.Code, exception.Message, exception.Retryable),
                cancellationToken).ConfigureAwait(false);
        }
        catch (JsonException)
        {
            await peer.SendResponseAsync(
                request,
                new { },
                new PipeError("invalid_payload", "The request payload is invalid."),
                cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception)
        {
            await peer.SendResponseAsync(
                request,
                new { },
                new PipeError("internal_error", "The local chat request failed.", Retryable: true),
                cancellationToken).ConfigureAwait(false);
        }
    }

    private static T Deserialize<T>(PipeEnvelope request) =>
        request.Payload.Deserialize<T>(PipeJson.SerializerOptions)
        ?? throw new JsonException($"{request.Method} payload is required.");

    private void RequireInitialized()
    {
        if (Volatile.Read(ref _initialized) == 0)
            throw new ChatRequestException("not_initialized", "initialize must be called first.");
    }
}

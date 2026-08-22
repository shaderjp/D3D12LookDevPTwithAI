using System.IO.Pipes;
using System.Security.Principal;
using D3D12LookDevPTwithAI.Chat.Core;

namespace D3D12LookDevPTwithAI.ChatHost;

public interface IPipePeer
{
    Task SendResponseAsync(
        PipeEnvelope request,
        object payload,
        PipeError? error = null,
        CancellationToken cancellationToken = default);

    Task SendEventAsync(
        Guid requestId,
        string method,
        object payload,
        CancellationToken cancellationToken = default);
}

public sealed class NamedPipeConnection : IPipePeer, IAsyncDisposable
{
    private readonly NamedPipeClientStream _stream;
    private readonly int _parentProcessId;
    private readonly SemaphoreSlim _writeGate = new(1, 1);
    private long _outgoingSequence;
    private long _incomingSequence;
    private int _disposed;

    public NamedPipeConnection(CommandLineOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        _parentProcessId = options.ParentProcessId;
        _stream = new NamedPipeClientStream(
            ".",
            options.PipeName,
            PipeDirection.InOut,
            PipeOptions.Asynchronous,
            TokenImpersonationLevel.Identification);
    }

    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        try
        {
            await _stream.ConnectAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            throw new TimeoutException("The native host did not accept the ChatHost pipe connection within 10 seconds.");
        }
        NativeMethods.VerifyNamedPipeServerProcess(_stream.SafePipeHandle, _parentProcessId);
    }

    public async ValueTask<PipeEnvelope?> ReadAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        var envelope = await PipeFraming.ReadAsync(_stream, cancellationToken).ConfigureAwait(false);
        if (envelope is null) return null;
        var previous = Interlocked.Read(ref _incomingSequence);
        if (envelope.Sequence <= previous)
            throw new PipeProtocolException("Incoming pipe sequence must be strictly increasing.");
        Interlocked.Exchange(ref _incomingSequence, envelope.Sequence);
        return envelope;
    }

    public Task SendResponseAsync(
        PipeEnvelope request,
        object payload,
        PipeError? error = null,
        CancellationToken cancellationToken = default) =>
        WriteAsync(
            PipeMessageKind.Response,
            request.RequestId,
            request.Method,
            PipeJson.ToElement(payload),
            error,
            cancellationToken);

    public Task SendEventAsync(
        Guid requestId,
        string method,
        object payload,
        CancellationToken cancellationToken = default) =>
        WriteAsync(
            PipeMessageKind.Event,
            requestId,
            method,
            PipeJson.ToElement(payload),
            error: null,
            cancellationToken);

    private async Task WriteAsync(
        PipeMessageKind kind,
        Guid requestId,
        string method,
        System.Text.Json.JsonElement payload,
        PipeError? error,
        CancellationToken cancellationToken)
    {
        await _writeGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var envelope = new PipeEnvelope
            {
                Kind = kind,
                RequestId = requestId,
                Sequence = ++_outgoingSequence,
                Method = method,
                Payload = payload,
                Error = error,
            };
            await PipeFraming.WriteAsync(_stream, envelope, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _writeGate.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        try
        {
            if (_stream.IsConnected)
                await _stream.DisposeAsync().ConfigureAwait(false);
            else
                _stream.Dispose();
        }
        finally
        {
            _writeGate.Dispose();
        }
    }
}

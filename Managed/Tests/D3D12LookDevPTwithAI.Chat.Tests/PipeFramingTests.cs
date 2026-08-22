using System.Buffers.Binary;
using D3D12LookDevPTwithAI.Chat.Core;

namespace D3D12LookDevPTwithAI.Chat.Tests;

public sealed class PipeFramingTests
{
    [Fact]
    public async Task Round_trips_a_length_prefixed_envelope_with_fragmented_reads()
    {
        var expected = new PipeEnvelope
        {
            Kind = PipeMessageKind.Request,
            RequestId = Guid.NewGuid(),
            Sequence = 7,
            Method = "initialize",
            Payload = PipeJson.ToElement(new InitializeRequest("instance", "project-key")),
        };
        await using var destination = new MemoryStream();
        await PipeFraming.WriteAsync(destination, expected);
        await using var source = new FragmentedReadStream(destination.ToArray(), maximumChunkBytes: 3);

        var actual = await PipeFraming.ReadAsync(source);

        Assert.NotNull(actual);
        Assert.Equal(expected.RequestId, actual.RequestId);
        Assert.Equal(expected.Sequence, actual.Sequence);
        Assert.Equal(expected.Method, actual.Method);
        Assert.Equal("instance", actual.Payload.GetProperty("instanceId").GetString());
        Assert.Null(await PipeFraming.ReadAsync(source));
    }

    [Fact]
    public async Task Rejects_a_frame_larger_than_four_mib_before_reading_its_body()
    {
        var prefix = new byte[sizeof(uint)];
        BinaryPrimitives.WriteUInt32LittleEndian(prefix, PipeProtocol.MaximumFrameBytes + 1u);
        await using var source = new MemoryStream(prefix);

        var exception = await Assert.ThrowsAsync<PipeProtocolException>(async () =>
            await PipeFraming.ReadAsync(source));

        Assert.Contains("frame length", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task Rejects_a_truncated_frame()
    {
        var bytes = new byte[sizeof(uint) + 2];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes, 10);
        await using var source = new MemoryStream(bytes);

        await Assert.ThrowsAsync<EndOfStreamException>(async () =>
            await PipeFraming.ReadAsync(source));
    }

    private sealed class FragmentedReadStream(byte[] bytes, int maximumChunkBytes) : Stream
    {
        private readonly MemoryStream _inner = new(bytes);

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => _inner.Length;
        public override long Position { get => _inner.Position; set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count) =>
            _inner.Read(buffer, offset, Math.Min(count, maximumChunkBytes));
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default) =>
            _inner.ReadAsync(buffer[..Math.Min(buffer.Length, maximumChunkBytes)], cancellationToken);
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        protected override void Dispose(bool disposing)
        {
            if (disposing) _inner.Dispose();
            base.Dispose(disposing);
        }
    }
}

using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;

namespace NetDebug;

sealed class AuthFailedException : Exception
{
    public AuthFailedException(string message) : base(message) { }
}

sealed class RemoteClient : IDisposable
{
    public event Action? Authenticated;
    public event Action<RemoteFrame>? FrameReceived;
    public event Action<string>? ClipboardReceived;
    public event Action<string, List<RemoteFileEntry>>? FileListReceived;
    public event Action<uint, string, ulong>? DownloadStarted;
    public event Action<uint, byte[]>? DownloadChunk;
    public event Action<uint, byte>? TransferFinished;
    public event Action<uint, byte, string>? TransferFailed;
    public event Action<double>? LatencyUpdated;

    readonly object _write = new();
    readonly object _gate = new();
    StreamSettings _settings = new();
    bool _authed;
    volatile bool _dead;
    TcpClient? _tcp;
    NetworkStream? _stream;
    uint _clipSeq;

    public async Task RunAsync(string host, int port, string password, StreamSettings settings, CancellationToken cancellation)
    {
        lock (_gate) _settings = settings;
        using var tcp = new TcpClient();
        _tcp = tcp;
        using var connectCts = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        connectCts.CancelAfter(TimeSpan.FromSeconds(5));
        await tcp.ConnectAsync(host, port, connectCts.Token).ConfigureAwait(false);
        tcp.NoDelay = true;
        tcp.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);
        await using var stream = tcp.GetStream();
        _stream = stream;

        var hello = new Writer();
        hello.Raw(Msg.Magic);
        hello.U16(1);
        if (!Send(Msg.Hello, hello.ToArray())) throw new IOException("发送握手失败");

        var challenge = ReadPacket();
        if (challenge.Type != Msg.Challenge || challenge.Payload.Length != 16)
            throw new IOException("服务端握手异常");

        using (var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(password)))
        {
            var mac = hmac.ComputeHash(challenge.Payload);
            if (!Send(Msg.Auth, mac)) throw new IOException("发送认证失败");
        }

        using var pingCts = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        var ping = Task.Run(() => PingLoop(pingCts.Token), CancellationToken.None);
        try
        {
            ReadLoop();
        }
        finally
        {
            pingCts.Cancel();
            try { ping.Wait(TimeSpan.FromMilliseconds(300)); } catch { }
            lock (_gate) _authed = false;
            _stream = null;
        }
    }

    public void UpdateSettings(StreamSettings settings)
    {
        bool authed;
        lock (_gate)
        {
            _settings = settings;
            authed = _authed;
        }
        if (authed) Send(Msg.StreamConfig, settings.ToPayload());
    }

    public bool SendMouse(ushort x, ushort y, byte button, byte action, short wheel)
    {
        var w = new Writer();
        w.U16(x);
        w.U16(y);
        w.U8(button);
        w.U8(action);
        w.I16(wheel);
        return Send(Msg.Mouse, w.ToArray());
    }

    public bool SendKey(ushort virtualKey, bool down)
    {
        var w = new Writer();
        w.U16(virtualKey);
        w.U8(down ? (byte)1 : (byte)0);
        w.U8(0);
        return Send(Msg.Key, w.ToArray());
    }

    public bool SendClipboard(string text)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        if (bytes.Length > Msg.MaxClipboard) return false;
        var w = new Writer();
        w.U32(++_clipSeq);
        w.U32((uint)bytes.Length);
        if (bytes.Length > 0) w.Raw(bytes);
        return Send(Msg.Clipboard, w.ToArray());
    }

    public bool RequestFileList(string? path = null)
    {
        if (string.IsNullOrEmpty(path)) return Send(Msg.ListRequest, Array.Empty<byte>());
        var bytes = Encoding.UTF8.GetBytes(path);
        if (bytes.Length > 4000) return false;
        var w = new Writer();
        w.U16((ushort)bytes.Length);
        w.Raw(bytes);
        return Send(Msg.ListRequest, w.ToArray());
    }

    public bool RequestMkdir(string name)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        if (bytes.Length == 0 || bytes.Length > 4000) return false;
        var w = new Writer();
        w.U16((ushort)bytes.Length);
        w.Raw(bytes);
        return Send(Msg.FileMkdir, w.ToArray());
    }

    public bool RequestDelete(string name)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        if (bytes.Length > 4000) return false;
        var w = new Writer();
        w.U16((ushort)bytes.Length);
        w.Raw(bytes);
        return Send(Msg.FileDelete, w.ToArray());
    }

    public bool RequestDownload(string name)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        var w = new Writer();
        w.U16((ushort)bytes.Length);
        w.Raw(bytes);
        return Send(Msg.FileRequest, w.ToArray());
    }

    public bool BeginUpload(uint id, string name, ulong size)
    {
        var bytes = Encoding.UTF8.GetBytes(name);
        var w = new Writer();
        w.U32(id);
        w.U8(0);
        w.U8(0);
        w.U16((ushort)bytes.Length);
        w.U64(size);
        w.Raw(bytes);
        return Send(Msg.FileStart, w.ToArray());
    }

    public bool SendChunk(uint id, byte[] data)
    {
        var w = new Writer();
        w.U32(id);
        w.U32((uint)data.Length);
        w.Raw(data);
        return Send(Msg.FileChunk, w.ToArray());
    }

    public bool FinishUpload(uint id)
    {
        var w = new Writer();
        w.U32(id);
        w.U8(0);
        w.U8(0);
        return Send(Msg.FileDone, w.ToArray());
    }

    public bool Abort(uint id, byte direction)
    {
        var w = new Writer();
        w.U32(id);
        w.U8(direction);
        w.U8(0);
        w.U16(0);
        return Send(Msg.FileAbort, w.ToArray());
    }

    public void Dispose()
    {
        _dead = true;
        try { _tcp?.Close(); } catch { }
    }

    void PingLoop(CancellationToken cancellation)
    {
        while (!cancellation.IsCancellationRequested && !_dead)
        {
            if (cancellation.WaitHandle.WaitOne(1000)) return;
            bool authed;
            lock (_gate) authed = _authed;
            if (!authed) continue;
            var w = new Writer();
            w.U64((ulong)Stopwatch.GetTimestamp());
            if (!Send(Msg.Ping, w.ToArray())) return;
        }
    }

    void ReadLoop()
    {
        while (!_dead)
        {
            var packet = ReadPacket();
            var reader = new Reader(packet.Payload);
            switch (packet.Type)
            {
                case Msg.AuthOk:
                {
                    StreamSettings settings;
                    lock (_gate)
                    {
                        _authed = true;
                        settings = _settings;
                    }
                    Send(Msg.StreamConfig, settings.ToPayload());
                    Authenticated?.Invoke();
                    break;
                }
                case Msg.AuthFail:
                {
                    var reason = packet.Payload.Length > 0 ? packet.Payload[0] : (byte)1;
                    throw new AuthFailedException(reason == 2 ? "版本不符" : "密码错误");
                }
                case Msg.Frame:
                    EmitFrame(reader);
                    break;
                case Msg.Pong:
                {
                    var stamp = reader.U64();
                    if (!reader.Ok) break;
                    var ms = (Stopwatch.GetTimestamp() - (long)stamp) * 1000.0 / Stopwatch.Frequency;
                    if (ms is >= 0 and < 60000) LatencyUpdated?.Invoke(ms);
                    break;
                }
                case Msg.Clipboard:
                {
                    reader.U32();
                    var len = reader.U32();
                    if (!reader.Ok || len > Msg.MaxClipboard || len != reader.Remaining) return;
                    ClipboardReceived?.Invoke(reader.Str((int)len));
                    break;
                }
                case Msg.FileList:
                    EmitFileList(reader);
                    break;
                case Msg.FileStart:
                {
                    var id = reader.U32();
                    var direction = reader.U8();
                    reader.U8();
                    var nameLen = reader.U16();
                    var size = reader.U64();
                    var name = reader.Str(nameLen);
                    if (!reader.Ok) return;
                    if (direction == 1) DownloadStarted?.Invoke(id, name, size);
                    break;
                }
                case Msg.FileChunk:
                {
                    var id = reader.U32();
                    var len = reader.U32();
                    if (!reader.Ok || len > 65536 || len != reader.Remaining) return;
                    DownloadChunk?.Invoke(id, reader.Bytes((int)len));
                    break;
                }
                case Msg.FileDone:
                {
                    var id = reader.U32();
                    var direction = reader.U8();
                    if (!reader.Ok) return;
                    TransferFinished?.Invoke(id, direction);
                    break;
                }
                case Msg.FileAbort:
                case Msg.FileError:
                {
                    var id = reader.U32();
                    var direction = reader.U8();
                    reader.U8();
                    var msgLen = reader.U16();
                    var message = reader.Str(msgLen);
                    if (!reader.Ok) return;
                    TransferFailed?.Invoke(id, direction, message);
                    break;
                }
            }
        }
    }

    void EmitFrame(Reader reader)
    {
        var codec = reader.U8();
        reader.U8();
        var width = reader.U16();
        var height = reader.U16();
        var originW = reader.U16();
        var originH = reader.U16();
        var id = reader.U32();
        reader.U64();
        var dataLen = reader.U32();
        if (!reader.Ok || dataLen != reader.Remaining) return;
        FrameReceived?.Invoke(new RemoteFrame
        {
            Codec = codec,
            Width = width,
            Height = height,
            OriginWidth = originW,
            OriginHeight = originH,
            Id = id,
            Data = reader.Bytes((int)dataLen)
        });
    }

    void EmitFileList(Reader reader)
    {
        var rootLen = reader.U16();
        var root = reader.Str(rootLen);
        var count = reader.U16();
        var files = new List<RemoteFileEntry>(count);
        for (var i = 0; i < count; i++)
        {
            var kind = reader.U8();
            var size = reader.U64();
            var nameLen = reader.U16();
            var name = reader.Str(nameLen);
            if (!reader.Ok) return;
            files.Add(new RemoteFileEntry { Name = name, Size = size, IsDirectory = kind == 1 });
        }
        FileListReceived?.Invoke(root, files);
    }

    bool Send(byte type, byte[] payload)
    {
        if (_dead) return false;
        try
        {
            var packet = Writer.Packet(type, payload);
            lock (_write)
            {
                if (_dead || _stream == null) return false;
                _stream.Write(packet, 0, packet.Length);
            }
            return true;
        }
        catch
        {
            try { _tcp?.Close(); } catch { }
            return false;
        }
    }

    (byte Type, byte[] Payload) ReadPacket()
    {
        var header = ReadExact(5);
        var len = (int)BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(1));
        if (len < 0 || len > Msg.MaxPayload) throw new IOException("消息过长");
        var payload = len == 0 ? Array.Empty<byte>() : ReadExact(len);
        return (header[0], payload);
    }

    byte[] ReadExact(int length)
    {
        var buffer = new byte[length];
        var got = 0;
        var stream = _stream ?? throw new IOException("连接已断开");
        while (got < length)
        {
            var n = stream.Read(buffer, got, length - got);
            if (n <= 0) throw new IOException("连接已断开");
            got += n;
        }
        return buffer;
    }
}

using System.Buffers.Binary;
using System.IO;
using System.Text;

namespace NetDebug;

static class Msg
{
    public const byte Hello = 1;
    public const byte Challenge = 2;
    public const byte Auth = 3;
    public const byte AuthOk = 4;
    public const byte AuthFail = 5;
    public const byte StreamConfig = 6;
    public const byte Frame = 7;
    public const byte Mouse = 8;
    public const byte Key = 9;
    public const byte Ping = 10;
    public const byte Pong = 11;
    public const byte Clipboard = 12;
    public const byte FileStart = 13;
    public const byte FileChunk = 14;
    public const byte FileDone = 15;
    public const byte FileAbort = 16;
    public const byte FileError = 17;
    public const byte ListRequest = 18;
    public const byte FileList = 19;
    public const byte FileRequest = 20;
    public const byte FileDelete = 21;
    public const byte FileMkdir = 22;

    public const int MaxPayload = 32 * 1024 * 1024;
    public const int MaxClipboard = 1024 * 1024;
    public const int ChunkSize = 32 * 1024;
    public static readonly byte[] Magic = "NDBG0001"u8.ToArray();
}

sealed class StreamSettings
{
    public byte Codec { get; init; }
    public byte JpegQuality { get; init; } = 30;
    public ushort MaxWidth { get; init; } = 1280;
    public byte MaxFps { get; init; } = 30;

    public byte[] ToPayload()
    {
        var w = new Writer();
        w.U8(Codec);
        w.U8(JpegQuality);
        w.U16(MaxWidth);
        w.U8(MaxFps);
        w.U8(0);
        return w.ToArray();
    }
}

sealed class RemoteFrame
{
    public byte Codec { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int OriginWidth { get; init; }
    public int OriginHeight { get; init; }
    public uint Id { get; init; }
    public byte[] Data { get; init; } = Array.Empty<byte>();
}

sealed class RemoteFileEntry
{
    public string Name { get; init; } = "";
    public ulong Size { get; init; }
    public bool IsDirectory { get; init; }
    public override string ToString() => Name + "    " + FormatSize(Size);

    public static string FormatSize(ulong n)
    {
        if (n < 1024) return n + " B";
        if (n < 1024 * 1024) return (n / 1024.0).ToString("0.#") + " KB";
        if (n < 1024UL * 1024 * 1024) return (n / 1024.0 / 1024).ToString("0.#") + " MB";
        return (n / 1024.0 / 1024 / 1024).ToString("0.##") + " GB";
    }
}

sealed class Writer
{
    readonly MemoryStream _stream = new();

    public void U8(byte value) => _stream.WriteByte(value);

    public void U16(ushort value)
    {
        Span<byte> buf = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16LittleEndian(buf, value);
        _stream.Write(buf);
    }

    public void U32(uint value)
    {
        Span<byte> buf = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(buf, value);
        _stream.Write(buf);
    }

    public void U64(ulong value)
    {
        Span<byte> buf = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(buf, value);
        _stream.Write(buf);
    }

    public void I16(short value) => U16(unchecked((ushort)value));

    public void Raw(ReadOnlySpan<byte> data) => _stream.Write(data);

    public byte[] ToArray() => _stream.ToArray();

    public static byte[] Packet(byte type, byte[] payload)
    {
        var msg = new byte[5 + payload.Length];
        msg[0] = type;
        BinaryPrimitives.WriteUInt32LittleEndian(msg.AsSpan(1), (uint)payload.Length);
        payload.CopyTo(msg.AsSpan(5));
        return msg;
    }
}

sealed class Reader
{
    readonly byte[] _data;
    int _index;

    public Reader(byte[] data) => _data = data;
    public bool Ok { get; private set; } = true;
    public int Remaining => Math.Max(0, _data.Length - _index);

    public byte U8()
    {
        if (_index + 1 > _data.Length)
        {
            Ok = false;
            return 0;
        }
        return _data[_index++];
    }

    public ushort U16()
    {
        if (_index + 2 > _data.Length)
        {
            Ok = false;
            return 0;
        }
        var value = BinaryPrimitives.ReadUInt16LittleEndian(_data.AsSpan(_index));
        _index += 2;
        return value;
    }

    public uint U32()
    {
        var lo = (uint)U16();
        var hi = (uint)U16();
        return lo | (hi << 16);
    }

    public ulong U64()
    {
        var lo = (ulong)U32();
        var hi = (ulong)U32();
        return lo | (hi << 32);
    }

    public short I16() => unchecked((short)U16());

    public string Str(int length)
    {
        var bytes = Bytes(length);
        return Ok ? Encoding.UTF8.GetString(bytes) : "";
    }

    public byte[] Bytes(int length)
    {
        if (length < 0 || _index > _data.Length || length > _data.Length - _index)
        {
            Ok = false;
            return Array.Empty<byte>();
        }
        var data = new byte[length];
        Buffer.BlockCopy(_data, _index, data, 0, length);
        _index += length;
        return data;
    }
}

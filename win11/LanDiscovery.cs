using System.Buffers.Binary;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Numerics;

namespace NetDebug;

static class LanDiscovery
{
    public static async Task<List<string>> FindAsync(int port, IProgress<string>? progress, CancellationToken token)
    {
        var hosts = HostsOnLocalSubnets();
        var found = new System.Collections.Concurrent.ConcurrentBag<string>();
        var done = 0;
        var options = new ParallelOptions
        {
            MaxDegreeOfParallelism = 48,
            CancellationToken = token
        };
        await Parallel.ForEachAsync(hosts, options, async (host, ct) =>
        {
            if (await IsNetDebug(host, port, ct)) found.Add(host);
            var finished = Interlocked.Increment(ref done);
            if (finished == hosts.Count || finished % 32 == 0)
                progress?.Report($"正在搜索 {finished}/{hosts.Count}");
        });
        return found
            .Distinct(StringComparer.Ordinal)
            .OrderBy(AddressValue)
            .ToList();
    }

    static async Task<bool> IsNetDebug(string host, int port, CancellationToken token)
    {
        using var tcp = new TcpClient();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(token);
        timeout.CancelAfter(TimeSpan.FromMilliseconds(400));
        try
        {
            await tcp.ConnectAsync(host, port, timeout.Token);
            var stream = tcp.GetStream();
            var hello = new Writer();
            hello.Raw(Msg.Magic);
            hello.U16(1);
            var packet = Writer.Packet(Msg.Hello, hello.ToArray());
            await stream.WriteAsync(packet, timeout.Token);
            var header = new byte[5];
            await ReadExact(stream, header, timeout.Token);
            if (header[0] != Msg.Challenge) return false;
            return BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(1)) == 16;
        }
        catch
        {
            return false;
        }
    }

    static async Task ReadExact(NetworkStream stream, byte[] buffer, CancellationToken token)
    {
        var got = 0;
        while (got < buffer.Length)
        {
            var n = await stream.ReadAsync(buffer.AsMemory(got), token);
            if (n == 0) throw new IOException("连接已关闭");
            got += n;
        }
    }

    static List<string> HostsOnLocalSubnets()
    {
        var self = new HashSet<string>(StringComparer.Ordinal);
        var hosts = new HashSet<string>(StringComparer.Ordinal);
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up) continue;
            if (nic.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
            foreach (var address in nic.GetIPProperties().UnicastAddresses)
            {
                if (address.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                var ip = address.Address.GetAddressBytes();
                var mask = address.IPv4Mask?.GetAddressBytes();
                if (ip.Length != 4 || mask == null || mask.Length != 4) continue;
                self.Add(address.Address.ToString());
                var prefix = mask.Sum(b => BitOperations.PopCount(b));
                if (prefix is < 1 or > 30) continue;
                if (prefix < 24)
                {
                    mask = [255, 255, 255, 0];
                }
                var ipValue = BinaryPrimitives.ReadUInt32BigEndian(ip);
                var maskValue = BinaryPrimitives.ReadUInt32BigEndian(mask);
                var network = ipValue & maskValue;
                var broadcast = network | ~maskValue;
                var bytes = new byte[4];
                for (var host = network + 1; host < broadcast; host++)
                {
                    BinaryPrimitives.WriteUInt32BigEndian(bytes, host);
                    hosts.Add(new IPAddress(bytes).ToString());
                }
            }
        }
        hosts.ExceptWith(self);
        return hosts.ToList();
    }

    static uint AddressValue(string text)
    {
        return IPAddress.TryParse(text, out var address)
            ? BinaryPrimitives.ReadUInt32BigEndian(address.GetAddressBytes())
            : uint.MaxValue;
    }
}

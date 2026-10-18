# NetDebug

局域网远程桌面。Ubuntu 22.04 作为被控端，Windows 作为控制端，双方直连，不经过中转服务器。

被控端是 C++ 服务，登录图形桌面后由 systemd 用户服务启动。控制端是 C# WPF 程序，编译后是一个 `NetDebug.exe`。

## 功能

- 查看 Ubuntu 桌面，并用鼠标、键盘操作
- 同步文本剪贴板
- 在左侧目录里浏览远程文件夹，拖放或选择文件双向传输，也可以删除
- 画面可选 JPEG 或原始 RGB24，并能设置 JPEG 质量、最大宽度和帧率上限
- 同一时间只接受一个客户端

默认监听 `0.0.0.0:17611`。密码用 HMAC-SHA256 校验，不会明文发送。首次启动生成 `~/.config/netdebug/config.ini`，默认密码是 `netdebug`。

## 环境

| | 要求 |
| --- | --- |
| 被控端 | Ubuntu 22.04，登录会话选择 **Ubuntu on Xorg** |
| 控制端 | Windows x64，[.NET 8 桌面运行时](https://dotnet.microsoft.com/download/dotnet/8.0) |

画面从 Xorg 读取。Wayland 会话里服务可以连上，但桌面是黑的。

## 安装被控端

在 Ubuntu 上用登录桌面的用户执行，第一次安装和以后更新都是这一条：

```bash
bash service/install.sh
```

脚本会安装编译依赖、编译并安装 `/usr/local/bin/netdebug-service`，然后启用用户服务。已有的 `~/.config/netdebug/config.ini` 会保留。

改完密码或端口后重启服务才会生效：

```bash
systemctl --user restart netdebug.service
```

查看日志：

```bash
journalctl --user -u netdebug.service -n 50 --no-pager
```

`config.ini` 示例：

```ini
port=17611
password=netdebug
download_dir=
```

`download_dir` 留空时，文件传输从 `~/Downloads` 开始。连上之后可以在客户端里切换目录。

## 编译控制端

在 Windows 上安装 .NET 8 SDK 后：

```bash
dotnet build -c Release win11/NetDebug.Client.csproj
```

生成的程序是 `win11/bin/Release/net8.0-windows/NetDebug.exe`。它依赖本机已安装的 .NET 8 桌面运行时。

## 使用

1. Ubuntu 登录时选择 **Ubuntu on Xorg**。
2. 确认 `netdebug.service` 已在运行。
3. 打开 `NetDebug.exe`，填写 Ubuntu 的 IP、端口 `17611` 和密码，然后连接。
4. 点击画面后，键盘输入才会发到 Ubuntu。按 `Ctrl+Alt` 把键盘交回 Windows。
5. 画质栏里的 **全屏** 会收起左侧目录，让画面占满其余区域。再点 **退出全屏** 把目录展开。

## 目录

```text
service/     Ubuntu 服务、安装脚本和 systemd 用户单元
win11/       Windows 客户端
```

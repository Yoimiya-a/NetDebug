# NetDebug

局域网远程桌面。Ubuntu 22.04 作为被控端，Windows 作为控制端，双方直连，不经过中转服务器。

被控端是 C++ 服务。安装脚本会把当前用户设为开机自动登录 Xorg 桌面，进入桌面后由 systemd 用户服务启动。控制端是 Qt 6 程序，界面、会话和协议分开，编译后是 `NetDebug.exe` 以及同目录的 Qt 运行库。

## 功能

- 查看 Ubuntu 桌面，并用鼠标、键盘操作
- 同步文本剪贴板
- 在左侧目录里浏览远程文件夹，拖放或选择文件双向传输，也可以删除
- 画面可选 JPEG 或原始 RGB24，并能设置 JPEG 质量、最大宽度和帧率上限
- 画面没变化时不发送。只有一小块变化时发送这一块，变化很大时仍发送整帧。服务端和这份客户端要一起更新，旧客户端收不到分块画面
- 同一时间只接受一个客户端
- 打开客户端后自动在局域网搜索设备。只找到一台时填入地址栏；找到多台时，从地址栏的下拉列表里选择。也可以再点 **搜索**

默认监听 `0.0.0.0:17611`。密码用 HMAC-SHA256 校验，不会明文发送。首次启动生成 `~/.config/netdebug/config.ini`，默认密码是 `netdebug`。

## 环境

| | 要求 |
| --- | --- |
| 被控端 | Ubuntu 22.04。安装后开机自动登录 **Ubuntu on Xorg** |
| 控制端 | Windows x64。运行时不需要另装 .NET，把编译出的整个目录拷过去即可 |

画面从 Xorg 读取。Wayland 会话里服务可以连上，但桌面是黑的。

## 安装被控端

在 Ubuntu 上用登录桌面的用户执行，第一次安装和以后更新都是这一条：

```bash
bash service/install.sh
```

脚本会安装编译依赖、编译并安装 `/usr/local/bin/netdebug-service`，启用用户服务，并把当前用户设为开机自动登录 Xorg 桌面。已有的 `~/.config/netdebug/config.ini` 会保留。装完后重启一次，之后不用在被控机前输入密码。

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

## 下载客户端

已经发布的 [v0.2](https://github.com/Yoimiya-a/NetDebug/releases/tag/v0.2) 仍是旧的 .NET 版本。当前源码里的控制端改成了 Qt，不再使用 .NET。编译结束会把 Qt 和 MinGW 运行库放到 `NetDebug.exe` 旁边。换一台电脑时要拷贝整个输出目录，不能只拷这一个 exe。对方不用安装 .NET，也不用安装 Qt。

## 编译控制端

需要 Qt 6（Widgets、Network、Concurrent）和 CMake。下面的路径按本机 Qt 安装位置修改：

```bash
cmake -S win11 -B win11/build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.7.1/mingw_64"
cmake --build win11/build
```

生成的程序是 `win11/build/NetDebug.exe`。协议在 `win11/src/protocol`，连接和局域网搜索在 `win11/src/session`，窗口在 `win11/src/ui`。

## 使用

1. 在被控机上运行过 `service/install.sh` 后重启。开机后会自动进入 Xorg 桌面并启动服务，不用在本机登录。
2. 确认 `netdebug.service` 已在运行。
3. 打开 `NetDebug.exe`。客户端会在局域网里搜索正在监听 `17611` 的设备：找到一台就写入窗口底部的地址栏，找到多台就在地址栏下拉里选择。地址也可以手填。填好端口 `17611` 和密码后连接。
4. 点击画面后，键盘输入才会发到 Ubuntu。按 `Ctrl+Alt` 把键盘交回 Windows。
5. 顶部 **画面** 菜单里的 **全屏** 会收起左侧目录和下边栏，让画面占满其余区域。延迟、收帧和连接状态平时在下边栏，全屏后改到顶栏右侧，退出全屏后再回到下边栏。再点 **退出全屏** 把目录和下边栏展开。

## 目录

```text
service/     Ubuntu 服务、安装脚本和 systemd 用户单元
win11/       Windows 客户端（Qt）
```

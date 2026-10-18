using System.Collections.ObjectModel;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace NetDebug;

public partial class MainWindow : Window
{
    const int WmClipboardUpdate = 0x031D;

    readonly DispatcherTimer _settingsTimer = new() { Interval = TimeSpan.FromMilliseconds(120) };
    readonly object _frameLock = new();
    readonly object _fileLock = new();
    readonly HashSet<int> _keysDown = new();

    RemoteClient? _client;
    CancellationTokenSource? _loopCts;
    Task? _loop;
    RemoteFrame? _pendingFrame;
    int _framePosted;
    WriteableBitmap? _rawBitmap;

    bool _uiReady;
    bool _connected;
    bool _connecting;
    bool _autoReconnect = true;
    bool _keyboard;
    bool _screenClick;
    bool _ignoreClipboard;
    bool _cancelTransfer;
    int _originW;
    int _originH;
    int _framesInWindow;
    DateTime _fpsWindow = DateTime.UtcNow;
    string _lastRemoteClipboard = "\u0001";
    string _lastSentClipboard = "\u0001";
    uint _uploadId;
    uint _downloadId;
    ulong _downloadGot;
    ulong _downloadSize;
    int _nextUploadId = 1;
    string _downloadPath = "";
    bool _downloadOpened;
    string _remoteRoot = "";
    FileStream? _downloadStream;
    TaskCompletionSource<bool>? _uploadWait;
    TaskCompletionSource<bool>? _downloadWait;
    IntPtr _hwnd;

    public MainWindow()
    {
        InitializeComponent();
        _settingsTimer.Tick += (_, _) =>
        {
            _settingsTimer.Stop();
            if (_connected) _client?.UpdateSettings(ReadSettings());
        };
        _uiReady = true;
        UpdateBandwidth();
        UpdateUi();
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        if (PresentationSource.FromVisual(this) is HwndSource source)
        {
            _hwnd = source.Handle;
            source.AddHook(WndProc);
            AddClipboardFormatListener(_hwnd);
        }
    }

    protected override void OnClosed(EventArgs e)
    {
        if (_hwnd != IntPtr.Zero) RemoveClipboardFormatListener(_hwnd);
        _loopCts?.Cancel();
        _client?.Dispose();
        lock (_fileLock)
        {
            _downloadStream?.Dispose();
            _downloadStream = null;
        }
        base.OnClosed(e);
    }

    void Connect_Click(object sender, RoutedEventArgs e)
    {
        if (_loop != null) return;
        var host = HostBox.Text.Trim();
        if (host.Length == 0)
        {
            StatusText.Text = "请填写地址";
            return;
        }
        if (!int.TryParse(PortBox.Text.Trim(), out var port) || port is < 1 or > 65535)
        {
            StatusText.Text = "端口无效";
            return;
        }

        var password = PasswordBox.Password;
        _connecting = true;
        _cancelTransfer = false;
        StatusText.Text = "正在连接";
        UpdateUi();
        _loopCts = new CancellationTokenSource();
        var token = _loopCts.Token;
        _loop = Task.Run(() => ConnectLoop(host, port, password, token));
    }

    async Task ConnectLoop(string host, int port, string password, CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            StreamSettings settings;
            try
            {
                settings = await Dispatcher.InvokeAsync(ReadSettings);
            }
            catch
            {
                break;
            }

            var client = new RemoteClient();
            Wire(client);
            _client = client;
            try
            {
                await client.RunAsync(host, port, password, settings, token).ConfigureAwait(false);
                await Dispatcher.InvokeAsync(() => StatusText.Text = "连接已断开");
            }
            catch (AuthFailedException ex)
            {
                await Dispatcher.InvokeAsync(() => StatusText.Text = ex.Message);
                client.Dispose();
                break;
            }
            catch (Exception ex) when (ex is IOException or SocketException or OperationCanceledException or ObjectDisposedException)
            {
                client.Dispose();
                if (token.IsCancellationRequested) break;
                var message = ex.Message;
                await Dispatcher.InvokeAsync(() => StatusText.Text = "重连中：" + message);
            }
            finally
            {
                if (ReferenceEquals(_client, client)) _client = null;
                client.Dispose();
            }

            try
            {
                await Dispatcher.InvokeAsync(() =>
                {
                    _connected = false;
                    _keyboard = false;
                    FailPendingTransfers();
                    UpdateCaptureHint();
                    UpdateUi();
                });
            }
            catch
            {
                break;
            }

            if (token.IsCancellationRequested || !_autoReconnect) break;
            try { await Task.Delay(2000, token).ConfigureAwait(false); }
            catch { break; }
        }

        try
        {
            await Dispatcher.InvokeAsync(() =>
            {
                _connecting = false;
                _connected = false;
                _keyboard = false;
                _loop = null;
                FailPendingTransfers();
                if (StatusText.Text.StartsWith("重连中") || StatusText.Text is "连接已断开" or "正在连接")
                    StatusText.Text = "未连接";
                UpdateCaptureHint();
                UpdateUi();
            });
        }
        catch
        {
            // 窗口已经关闭
        }
    }

    void Wire(RemoteClient client)
    {
        client.Authenticated += () => Dispatcher.BeginInvoke(() =>
        {
            _connected = true;
            StatusText.Text = "已连接";
            HintText.Text = "等待画面";
            HintText.Visibility = Visibility.Visible;
            LatencyText.Text = "延迟 --";
            FpsText.Text = "收帧 --";
            _framesInWindow = 0;
            _fpsWindow = DateTime.UtcNow;
            UpdateCaptureHint();
            UpdateUi();
            client.RequestFileList();
        });
        client.FrameReceived += OnFrame;
        client.ClipboardReceived += text => Dispatcher.BeginInvoke(() => ApplyRemoteClipboard(text));
        client.FileListReceived += (root, files) => Dispatcher.BeginInvoke(() => ShowRemoteFiles(root, files));
        client.DownloadStarted += OnDownloadStarted;
        client.DownloadChunk += OnDownloadChunk;
        client.TransferFinished += (id, direction) => Dispatcher.BeginInvoke(() => OnTransferFinished(id, direction));
        client.TransferFailed += (id, direction, message) => Dispatcher.BeginInvoke(() => OnTransferFailed(id, direction, message));
        client.LatencyUpdated += ms => Dispatcher.BeginInvoke(() => LatencyText.Text = $"延迟 {ms:0} ms");
    }

    void Disconnect_Click(object sender, RoutedEventArgs e)
    {
        _loopCts?.Cancel();
        _client?.Dispose();
        ReleaseKeyboard();
    }

    void ReconnectBox_Changed(object sender, RoutedEventArgs e)
    {
        if (!_uiReady) return;
        _autoReconnect = ReconnectBox.IsChecked == true;
    }

    void OnSettingsChanged(object sender, SelectionChangedEventArgs e) => ApplySettingsChange();

    void OnQualityChanged(object sender, RoutedPropertyChangedEventArgs<double> e) => ApplySettingsChange();

    void FullScreen_Click(object sender, RoutedEventArgs e)
    {
        var expanded = FilePanel.Visibility == Visibility.Visible;
        FilePanel.Visibility = expanded ? Visibility.Collapsed : Visibility.Visible;
        FullScreenButton.Content = expanded ? "退出全屏" : "全屏";
    }

    void ApplySettingsChange()
    {
        if (!_uiReady) return;
        QualityText.Text = ((int)QualitySlider.Value).ToString();
        QualitySlider.IsEnabled = TagInt(CodecBox, 0) == 0;
        UpdateBandwidth();
        _settingsTimer.Stop();
        _settingsTimer.Start();
    }

    StreamSettings ReadSettings()
    {
        var codec = (byte)TagInt(CodecBox, 0);
        var quality = (byte)Math.Clamp((int)QualitySlider.Value, 1, 100);
        var width = (ushort)Math.Clamp(TagInt(WidthBox, 1280), 0, 65535);
        var fps = (byte)Math.Clamp(TagInt(FpsBox, 30), 1, 60);
        return new StreamSettings
        {
            Codec = codec,
            JpegQuality = quality,
            MaxWidth = width,
            MaxFps = fps
        };
    }

    void UpdateBandwidth()
    {
        var codec = TagInt(CodecBox, 0);
        var quality = Math.Clamp((int)QualitySlider.Value, 1, 100);
        var maxWidth = TagInt(WidthBox, 1280);
        var fps = Math.Clamp(TagInt(FpsBox, 30), 1, 60);
        var originW = _originW > 0 ? _originW : 1920;
        var originH = _originH > 0 ? _originH : 1080;
        var width = maxWidth == 0 ? originW : Math.Min(maxWidth, originW);
        var height = Math.Max(1, originH * width / originW);
        if (codec == 0)
        {
            width &= ~1;
            height &= ~1;
        }
        if (width < 2) width = 2;
        if (height < 2) height = 2;

        bool warn;
        string text;
        if (codec == 0)
        {
            var mid = width * (double)height * 0.25 * (quality / 100.0) * fps * 8 / 1_000_000.0;
            var low = mid * 0.5;
            var high = mid * 3;
            warn = high > 80;
            text = $"预计 {low:0.#}–{high:0.#} Mbps";
        }
        else
        {
            var mbps = width * (double)height * 3 * fps * 8 / 1_000_000.0;
            warn = mbps > 80;
            text = $"预计 {mbps:0.#} Mbps";
        }
        if (_originW == 0) text += "（按 1920×1080 估算）";
        if (warn) text += "，可能超过百兆";
        BandwidthText.Text = text;
        BandwidthText.Foreground = Brushes.Black;
    }

    void OnFrame(RemoteFrame frame)
    {
        lock (_frameLock) _pendingFrame = frame;
        if (Interlocked.CompareExchange(ref _framePosted, 1, 0) == 0)
            Dispatcher.BeginInvoke(PresentPending);
    }

    void PresentPending()
    {
        RemoteFrame? frame;
        lock (_frameLock)
        {
            frame = _pendingFrame;
            _pendingFrame = null;
            _framePosted = 0;
        }
        if (frame != null) ShowFrame(frame);
        lock (_frameLock)
        {
            if (_pendingFrame != null && Interlocked.CompareExchange(ref _framePosted, 1, 0) == 0)
                Dispatcher.BeginInvoke(PresentPending);
        }
    }

    void ShowFrame(RemoteFrame frame)
    {
        if (frame.OriginWidth != _originW || frame.OriginHeight != _originH)
        {
            _originW = frame.OriginWidth;
            _originH = frame.OriginHeight;
            UpdateBandwidth();
        }

        if (frame.Codec == 0)
        {
            var image = DecodeJpeg(frame.Data);
            if (image == null) return;
            ScreenImage.Source = image;
        }
        else
        {
            var stride = frame.Width * 3;
            if (frame.Width <= 0 || frame.Height <= 0 || frame.Data.Length < stride * frame.Height) return;
            if (_rawBitmap == null || _rawBitmap.PixelWidth != frame.Width || _rawBitmap.PixelHeight != frame.Height)
                _rawBitmap = new WriteableBitmap(frame.Width, frame.Height, 96, 96, PixelFormats.Rgb24, null);
            _rawBitmap.WritePixels(new Int32Rect(0, 0, frame.Width, frame.Height), frame.Data, stride, 0);
            ScreenImage.Source = _rawBitmap;
        }

        HintText.Visibility = Visibility.Collapsed;
        var now = DateTime.UtcNow;
        _framesInWindow++;
        if ((now - _fpsWindow).TotalSeconds >= 1)
        {
            FpsText.Text = $"收帧 {_framesInWindow} fps";
            _framesInWindow = 0;
            _fpsWindow = now;
        }
    }

    static BitmapSource? DecodeJpeg(byte[] data)
    {
        try
        {
            using var stream = new MemoryStream(data);
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.StreamSource = stream;
            image.EndInit();
            image.Freeze();
            return image;
        }
        catch
        {
            return null;
        }
    }

    void Screen_MouseMove(object sender, MouseEventArgs e)
    {
        if (!_connected || !TryMap(e.GetPosition(ScreenImage), out var x, out var y, allowOutside: ScreenHost.IsMouseCaptured))
            return;
        _client?.SendMouse(x, y, 0, 0, 0);
    }

    void Screen_MouseDown(object sender, MouseButtonEventArgs e)
    {
        _screenClick = true;
        _keyboard = true;
        InputMethod.SetIsInputMethodEnabled(this, false);
        Keyboard.Focus(ScreenHost);
        UpdateCaptureHint();
        Dispatcher.BeginInvoke(DispatcherPriority.Input, () =>
        {
            if (_keyboard) Keyboard.Focus(ScreenHost);
            _screenClick = false;
        });
        ScreenHost.CaptureMouse();
        if (!_connected || !TryMap(e.GetPosition(ScreenImage), out var x, out var y, allowOutside: true)) return;
        var button = ButtonCode(e.ChangedButton);
        if (button == 0) return;
        _client?.SendMouse(x, y, button, 1, 0);
        e.Handled = true;
    }

    void Screen_MouseUp(object sender, MouseButtonEventArgs e)
    {
        if (TryMap(e.GetPosition(ScreenImage), out var x, out var y, allowOutside: true))
        {
            var button = ButtonCode(e.ChangedButton);
            if (button != 0) _client?.SendMouse(x, y, button, 2, 0);
        }
        if (Mouse.LeftButton != MouseButtonState.Pressed &&
            Mouse.RightButton != MouseButtonState.Pressed &&
            Mouse.MiddleButton != MouseButtonState.Pressed)
            ScreenHost.ReleaseMouseCapture();
    }

    void Screen_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (!_connected || !TryMap(e.GetPosition(ScreenImage), out var x, out var y, allowOutside: false)) return;
        var notches = e.Delta / 120;
        if (notches == 0) notches = e.Delta > 0 ? 1 : -1;
        _client?.SendMouse(x, y, 0, 3, (short)notches);
    }

    void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (!_keyboard || !_connected) return;
        if (Keyboard.FocusedElement is TextBox || Keyboard.FocusedElement is PasswordBox) return;
        if (Keyboard.FocusedElement is DependencyObject focused && IsUnder(focused, FileTree)) return;
        var vk = EventVirtualKey(e);
        if (vk <= 0) return;
        var ctrl = Keyboard.IsKeyDown(Key.LeftCtrl) || Keyboard.IsKeyDown(Key.RightCtrl);
        var alt = Keyboard.IsKeyDown(Key.LeftAlt) || Keyboard.IsKeyDown(Key.RightAlt) || e.Key == Key.System;
        if (ctrl && alt)
        {
            ReleaseKeyboard();
            e.Handled = true;
            return;
        }
        _keysDown.Add(vk);
        _client?.SendKey((ushort)vk, true);
        e.Handled = true;
    }

    void Window_PreviewKeyUp(object sender, KeyEventArgs e)
    {
        var vk = EventVirtualKey(e);
        if (vk <= 0 || !_keysDown.Remove(vk)) return;
        _client?.SendKey((ushort)vk, false);
        e.Handled = true;
    }

    void ReleaseKeyboard()
    {
        if (_keysDown.Count > 0 && _client != null)
        {
            foreach (var vk in _keysDown.ToArray()) _client.SendKey((ushort)vk, false);
        }
        _keysDown.Clear();
        _keyboard = false;
        InputMethod.SetIsInputMethodEnabled(this, true);
        UpdateCaptureHint();
    }

    static int EventVirtualKey(KeyEventArgs e)
    {
        var key = e.Key switch
        {
            Key.System => e.SystemKey,
            Key.ImeProcessed => e.ImeProcessedKey,
            Key.DeadCharProcessed => e.DeadCharProcessedKey,
            _ => e.Key
        };
        var vk = KeyInterop.VirtualKeyFromKey(key);
        if (vk <= 0 && e.ImeProcessedKey != Key.None) vk = KeyInterop.VirtualKeyFromKey(e.ImeProcessedKey);
        if (vk <= 0 && e.SystemKey != Key.None) vk = KeyInterop.VirtualKeyFromKey(e.SystemKey);
        return vk;
    }

    void UpdateCaptureHint()
    {
        CaptureHint.Visibility = _connected ? Visibility.Visible : Visibility.Collapsed;
        CaptureHint.Text = _keyboard
            ? "键盘控制中，Ctrl+Alt 交回本机"
            : "点击画面后键盘才会发到对方，Ctrl+Alt 交回本机";
    }

    bool TryMap(Point point, out ushort x, out ushort y, bool allowOutside)
    {
        x = 0;
        y = 0;
        if (ScreenImage.Source is not BitmapSource bitmap) return false;
        var frameW = bitmap.PixelWidth;
        var frameH = bitmap.PixelHeight;
        if (frameW < 1 || frameH < 1 || ScreenImage.ActualWidth < 1 || ScreenImage.ActualHeight < 1) return false;
        var scale = Math.Min(ScreenImage.ActualWidth / frameW, ScreenImage.ActualHeight / frameH);
        var drawnW = frameW * scale;
        var drawnH = frameH * scale;
        var offsetX = (ScreenImage.ActualWidth - drawnW) / 2;
        var offsetY = (ScreenImage.ActualHeight - drawnH) / 2;
        var frameX = (point.X - offsetX) / drawnW * frameW;
        var frameY = (point.Y - offsetY) / drawnH * frameH;
        var inside = frameX >= 0 && frameY >= 0 && frameX < frameW && frameY < frameH;
        if (!inside && !allowOutside) return false;
        frameX = Math.Clamp(frameX, 0, frameW - 1);
        frameY = Math.Clamp(frameY, 0, frameH - 1);
        var originW = _originW > 0 ? _originW : frameW;
        var originH = _originH > 0 ? _originH : frameH;
        var desktopX = (int)(frameX / frameW * originW);
        var desktopY = (int)(frameY / frameH * originH);
        x = (ushort)Math.Clamp(desktopX, 0, 65535);
        y = (ushort)Math.Clamp(desktopY, 0, 65535);
        return true;
    }

    static byte ButtonCode(MouseButton button) => button switch
    {
        MouseButton.Left => 1,
        MouseButton.Right => 2,
        MouseButton.Middle => 3,
        _ => 0
    };

    void Refresh_Click(object sender, RoutedEventArgs e) => _client?.RequestFileList();

    void Delete_Click(object sender, RoutedEventArgs e)
    {
        if (_client == null || !_connected || FileTree.SelectedItem is not FileNode node) return;
        if (node.RelativePath.Length == 0) return;
        var full = RemoteFullPath(node.RelativePath);
        var question = node.IsDirectory
            ? "删除这个文件夹以及里面的内容？\n" + full
            : "删除这个文件？\n" + full;
        if (MessageBox.Show(question, "删除", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
            return;
        if (!_client.RequestDelete(node.RelativePath)) TransferText.Text = "删除请求发送失败";
    }

    void FileTree_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Delete) return;
        e.Handled = true;
        Delete_Click(sender, e);
    }

    static bool IsUnder(DependencyObject element, DependencyObject root)
    {
        while (true)
        {
            if (element == root) return true;
            var next = VisualTreeHelper.GetParent(element) ?? LogicalTreeHelper.GetParent(element);
            if (next == null) return false;
            element = next;
        }
    }

    void Download_Click(object sender, RoutedEventArgs e)
    {
        if (FileTree.SelectedItem is not FileNode node) return;
        if (node.IsDirectory) _ = DownloadDirectory(node);
        else DownloadNode(node);
    }

    void FileTree_DoubleClick(object sender, MouseButtonEventArgs e)
    {
        if (FileTree.SelectedItem is not FileNode node) return;
        if (node.IsDirectory)
        {
            if (node.RelativePath.Length > 0) OpenRemote(RemoteFullPath(node.RelativePath));
            return;
        }
        DownloadNode(node);
    }

    void RemotePath_GotFocus(object sender, RoutedEventArgs e)
    {
        if (_screenClick) return;
        ReleaseKeyboard();
    }

    void RemotePath_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Enter) return;
        e.Handled = true;
        OpenTypedDirectory();
    }

    void Open_Click(object sender, RoutedEventArgs e) => OpenTypedDirectory();

    void Up_Click(object sender, RoutedEventArgs e)
    {
        if (_remoteRoot.Length <= 1) return;
        var slash = _remoteRoot.LastIndexOf('/');
        OpenRemote(slash <= 0 ? "/" : _remoteRoot[..slash]);
    }

    void OpenTypedDirectory()
    {
        var path = RemotePathBox.Text.Trim();
        if (path.Length == 0 || path == "未连接") return;
        OpenRemote(path);
    }

    void OpenRemote(string path)
    {
        if (_client == null || !_connected) return;
        if (!_client.RequestFileList(path)) TransferText.Text = "切换目录失败";
    }

    void FileTree_Selected(object sender, RoutedPropertyChangedEventArgs<object> e)
    {
        if (e.NewValue is not FileNode node || _remoteRoot.Length == 0)
        {
            RemotePlaceText.Text = "";
            return;
        }
        RemotePlaceText.Text = RemoteFullPath(node.RelativePath);
    }

    void FileTree_DragOver(object sender, DragEventArgs e)
    {
        e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    async void FileTree_Drop(object sender, DragEventArgs e)
    {
        if (_client == null || e.Data.GetData(DataFormats.FileDrop) is not string[] paths) return;
        e.Handled = true;
        await UploadLocals(paths, DropDirectory(e));
    }

    void Upload_Click(object sender, RoutedEventArgs e)
    {
        if (_client == null) return;
        var paths = UploadPicker.Pick(new WindowInteropHelper(this).Handle);
        if (paths == null || paths.Length == 0) return;
        _ = UploadLocals(paths, SelectedDirectory());
    }

    void DownloadNode(FileNode node)
    {
        if (_client == null || node.IsDirectory || node.RelativePath.Length == 0) return;
        var dialog = new SaveFileDialog
        {
            FileName = node.Name,
            Title = "保存到本机，远程位置 " + RemoteFullPath(node.RelativePath)
        };
        if (dialog.ShowDialog() != true) return;
        _cancelTransfer = false;
        _downloadOpened = false;
        _downloadPath = dialog.FileName;
        _downloadId = 0;
        TransferBar.Value = 0;
        TransferText.Text = "正在下载 " + RemoteFullPath(node.RelativePath);
        if (!_client.RequestDownload(node.RelativePath)) TransferText.Text = "下载请求发送失败";
    }

    async Task DownloadDirectory(FileNode node)
    {
        if (_client == null) return;
        var files = new List<FileNode>();
        CollectRemoteFiles(node, files);
        if (files.Count == 0)
        {
            TransferText.Text = "这个目录里没有文件";
            return;
        }
        var dialog = new OpenFolderDialog { Title = "保存到本机的文件夹" };
        if (dialog.ShowDialog() != true) return;
        _cancelTransfer = false;
        var saved = 0;
        foreach (var file in files)
        {
            if (_cancelTransfer || _client == null) break;
            var relative = LocalRelative(node, file);
            var local = Path.Combine(dialog.FolderName, relative.Replace('/', Path.DirectorySeparatorChar));
            var parent = Path.GetDirectoryName(local);
            if (!string.IsNullOrEmpty(parent)) Directory.CreateDirectory(parent);
            if (!await DownloadTo(file.RelativePath, local)) break;
            saved++;
        }
        if (_cancelTransfer) TransferText.Text = "已取消";
        else if (saved == files.Count) TransferText.Text = "已下载 " + saved + " 个文件到 " + dialog.FolderName;
    }

    async Task<bool> DownloadTo(string remotePath, string localPath)
    {
        if (_client == null) return false;
        var wait = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        _downloadWait = wait;
        _downloadOpened = false;
        _downloadPath = localPath;
        _downloadId = 0;
        TransferBar.Value = 0;
        TransferText.Text = "正在下载 " + RemoteFullPath(remotePath);
        if (!_client.RequestDownload(remotePath))
        {
            TransferText.Text = "下载请求发送失败";
            _downloadWait = null;
            return false;
        }
        return await wait.Task && !_cancelTransfer;
    }

    static void CollectRemoteFiles(FileNode node, List<FileNode> files)
    {
        if (!node.IsDirectory && node.RelativePath.Length > 0) files.Add(node);
        foreach (var child in node.Children) CollectRemoteFiles(child, files);
    }

    static string LocalRelative(FileNode folder, FileNode file)
    {
        if (folder.RelativePath.Length == 0) return file.RelativePath;
        var prefix = folder.RelativePath + "/";
        var tail = file.RelativePath.StartsWith(prefix, StringComparison.Ordinal)
            ? file.RelativePath[prefix.Length..]
            : file.Name;
        return folder.Name + "/" + tail;
    }

    async Task UploadLocals(IEnumerable<string> paths, string remoteDirectory)
    {
        if (_client == null) return;
        var jobs = new List<(string Local, string Remote)>();
        var directories = new List<string>();
        foreach (var path in paths) CollectUploads(path, remoteDirectory, directories, jobs);
        if (jobs.Count == 0 && directories.Count == 0)
        {
            TransferText.Text = "没有可上传的文件";
            return;
        }
        _cancelTransfer = false;
        foreach (var directory in directories.Distinct(StringComparer.Ordinal))
        {
            if (_cancelTransfer || _client == null) return;
            if (!_client.RequestMkdir(directory))
            {
                TransferText.Text = "创建文件夹失败";
                return;
            }
        }
        if (jobs.Count == 0)
        {
            TransferText.Text = "已上传文件夹";
            _client.RequestFileList();
            return;
        }
        var uploaded = 0;
        foreach (var job in jobs)
        {
            if (_cancelTransfer || _client == null) break;
            var size = (ulong)new FileInfo(job.Local).Length;
            var id = (uint)Interlocked.Increment(ref _nextUploadId);
            _uploadId = id;
            var wait = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            _uploadWait = wait;
            TransferBar.Value = 0;
            TransferText.Text = "正在上传到 " + RemoteFullPath(job.Remote);
            if (!_client.BeginUpload(id, job.Remote, size))
            {
                TransferText.Text = "上传请求发送失败";
                break;
            }

            var failed = false;
            await Task.Run(() =>
            {
                using var stream = File.OpenRead(job.Local);
                var buffer = new byte[Msg.ChunkSize];
                ulong sent = 0;
                int read;
                while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
                {
                    if (_cancelTransfer)
                    {
                        _client?.Abort(id, 0);
                        failed = true;
                        return;
                    }
                    var chunk = new byte[read];
                    Buffer.BlockCopy(buffer, 0, chunk, 0, read);
                    if (_client == null || !_client.SendChunk(id, chunk))
                    {
                        failed = true;
                        return;
                    }
                    sent += (ulong)read;
                    var percent = size == 0 ? 100 : sent * 100.0 / size;
                    Dispatcher.BeginInvoke(() => TransferBar.Value = percent);
                }
                _client?.FinishUpload(id);
            });

            if (failed || _cancelTransfer)
            {
                TransferText.Text = "已取消";
                break;
            }

            bool ok;
            try
            {
                ok = await wait.Task.WaitAsync(TimeSpan.FromSeconds(30));
            }
            catch
            {
                ok = false;
            }
            if (ok)
            {
                uploaded++;
                TransferBar.Value = 100;
                TransferText.Text = "已上传到 " + RemoteFullPath(job.Remote);
            }
            else if (TransferText.Text.StartsWith("正在上传"))
            {
                TransferText.Text = "上传未确认";
            }
        }
        if (uploaded > 1) TransferText.Text = "已上传 " + uploaded + " 个文件";
        _client?.RequestFileList();
    }

    static readonly EnumerationOptions UploadScan = new()
    {
        RecurseSubdirectories = true,
        IgnoreInaccessible = true,
        AttributesToSkip = FileAttributes.ReparsePoint
    };

    static void CollectUploads(string path, string remoteDirectory, List<string> directories, List<(string Local, string Remote)> jobs)
    {
        if (Directory.Exists(path))
        {
            var folder = Path.GetFileName(path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            if (folder.Length == 0 || folder == "." || folder == "..") return;
            var root = JoinRemote(remoteDirectory, folder);
            directories.Add(root);
            foreach (var dir in Directory.EnumerateDirectories(path, "*", UploadScan))
            {
                var rel = Path.GetRelativePath(path, dir).Replace('\\', '/');
                if (rel.Split('/').Any(part => part.Length == 0 || part == "." || part == "..")) continue;
                directories.Add(JoinRemote(remoteDirectory, folder + "/" + rel));
            }
            foreach (var file in Directory.EnumerateFiles(path, "*", UploadScan))
            {
                var rel = Path.GetRelativePath(path, file).Replace('\\', '/');
                if (rel.Split('/').Any(part => part.Length == 0 || part == "." || part == "..")) continue;
                jobs.Add((file, JoinRemote(remoteDirectory, folder + "/" + rel)));
            }
            return;
        }
        if (!File.Exists(path)) return;
        var name = Path.GetFileName(path);
        if (name.Length == 0 || name == "." || name == "..") return;
        jobs.Add((path, JoinRemote(remoteDirectory, name)));
    }

    void ShowRemoteFiles(string root, List<RemoteFileEntry> files)
    {
        var normalized = root.Replace('\\', '/').TrimEnd('/');
        _remoteRoot = normalized.Length == 0 ? "/" : normalized;
        RemotePathBox.Text = _remoteRoot;
        var rootName = _remoteRoot.Length == 0 ? "远程目录" : Path.GetFileName(_remoteRoot);
        if (rootName.Length == 0) rootName = _remoteRoot;
        var top = new FileNode { Name = rootName, RelativePath = "", IsDirectory = true, IsExpanded = true };
        foreach (var file in files.OrderBy(item => item.Name, StringComparer.OrdinalIgnoreCase))
        {
            var parts = file.Name.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries);
            var node = top;
            var rel = "";
            for (var i = 0; i < parts.Length; i++)
            {
                rel = rel.Length == 0 ? parts[i] : rel + "/" + parts[i];
                var last = i == parts.Length - 1;
                var child = node.Children.FirstOrDefault(item => item.Name == parts[i]);
                if (child == null)
                {
                    child = new FileNode
                    {
                        Name = parts[i],
                        RelativePath = rel,
                        IsDirectory = !last || file.IsDirectory,
                        Size = last && !file.IsDirectory ? file.Size : 0,
                        IsExpanded = false
                    };
                    node.Children.Add(child);
                }
                else if (last && !file.IsDirectory)
                {
                    child.IsDirectory = false;
                    child.Size = file.Size;
                }
                node = child;
            }
        }
        SortNodes(top);
        FileTree.ItemsSource = new[] { top };
        if (FileTree.SelectedItem is FileNode selected) RemotePlaceText.Text = RemoteFullPath(selected.RelativePath);
        else RemotePlaceText.Text = _remoteRoot;
        UpdateUi();
    }

    static void SortNodes(FileNode node)
    {
        var ordered = node.Children
            .OrderByDescending(item => item.IsDirectory)
            .ThenBy(item => item.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();
        node.Children.Clear();
        foreach (var child in ordered)
        {
            SortNodes(child);
            node.Children.Add(child);
        }
    }

    string SelectedDirectory()
    {
        if (FileTree.SelectedItem is not FileNode node) return "";
        return node.IsDirectory ? node.RelativePath : ParentPath(node.RelativePath);
    }

    string DropDirectory(DragEventArgs e)
    {
        var node = NodeAt(e.GetPosition(FileTree));
        if (node == null) return SelectedDirectory();
        return node.IsDirectory ? node.RelativePath : ParentPath(node.RelativePath);
    }

    FileNode? NodeAt(Point position)
    {
        var hit = FileTree.InputHitTest(position) as DependencyObject;
        while (hit != null && hit is not TreeViewItem) hit = VisualTreeHelper.GetParent(hit);
        return (hit as TreeViewItem)?.DataContext as FileNode;
    }

    string RemoteFullPath(string relative)
    {
        if (relative.Length == 0) return _remoteRoot;
        if (_remoteRoot.Length == 0 || _remoteRoot == "/") return "/" + relative.TrimStart('/');
        return _remoteRoot + "/" + relative.TrimStart('/');
    }

    static string ParentPath(string relative)
    {
        var slash = relative.LastIndexOf('/');
        return slash < 0 ? "" : relative[..slash];
    }

    static string JoinRemote(string directory, string name)
    {
        if (directory.Length == 0) return name.Replace('\\', '/');
        return directory.TrimEnd('/') + "/" + name.Replace('\\', '/').TrimStart('/');
    }

    void Cancel_Click(object sender, RoutedEventArgs e)
    {
        _cancelTransfer = true;
        if (_uploadId != 0) _client?.Abort(_uploadId, 0);
        if (_downloadId != 0) _client?.Abort(_downloadId, 1);
        CloseDownload(deletePartial: true);
        FailPendingTransfers();
        TransferText.Text = "已取消";
    }

    void FailPendingTransfers()
    {
        _uploadWait?.TrySetResult(false);
        _downloadWait?.TrySetResult(false);
    }

    void OnDownloadStarted(uint id, string name, ulong size)
    {
        try
        {
            var stream = File.Create(_downloadPath);
            lock (_fileLock)
            {
                _downloadStream?.Dispose();
                _downloadStream = stream;
                _downloadId = id;
                _downloadSize = size;
                _downloadGot = 0;
                _downloadOpened = true;
            }
        }
        catch (Exception ex)
        {
            _client?.Abort(id, 1);
            Dispatcher.BeginInvoke(() => TransferText.Text = ex.Message);
            return;
        }
        Dispatcher.BeginInvoke(() =>
        {
            TransferBar.Value = size == 0 ? 100 : 0;
            TransferText.Text = "正在下载 " + name;
        });
    }

    void OnDownloadChunk(uint id, byte[] data)
    {
        lock (_fileLock)
        {
            if (_downloadStream == null || id != _downloadId) return;
            _downloadStream.Write(data, 0, data.Length);
            _downloadGot += (ulong)data.Length;
        }
        var percent = _downloadSize == 0 ? 100 : _downloadGot * 100.0 / _downloadSize;
        Dispatcher.BeginInvoke(() => TransferBar.Value = Math.Min(100, percent));
    }

    void OnTransferFinished(uint id, byte direction)
    {
        if (direction == 0 && id == _uploadId)
        {
            _uploadWait?.TrySetResult(true);
            return;
        }
        if (direction == 1 && (id == _downloadId || id == 0))
        {
            CloseDownload(deletePartial: false);
            TransferBar.Value = 100;
            TransferText.Text = "下载完成";
            _downloadId = 0;
            _downloadWait?.TrySetResult(true);
        }
    }

    void OnTransferFailed(uint id, byte direction, string message)
    {
        if (direction == 2)
        {
            TransferText.Text = string.IsNullOrWhiteSpace(message) ? "打不开这个目录" : message;
            return;
        }
        if (direction == 0 && (id == _uploadId || id == 0)) _uploadWait?.TrySetResult(false);
        if (direction == 1)
        {
            CloseDownload(deletePartial: true);
            _downloadId = 0;
            _downloadWait?.TrySetResult(false);
        }
        TransferText.Text = string.IsNullOrWhiteSpace(message) ? "传输失败" : message;
    }

    void CloseDownload(bool deletePartial)
    {
        lock (_fileLock)
        {
            _downloadStream?.Dispose();
            _downloadStream = null;
        }
        if (deletePartial && _downloadOpened && !string.IsNullOrEmpty(_downloadPath))
        {
            try { File.Delete(_downloadPath); } catch { }
        }
        _downloadOpened = false;
    }

    void ApplyRemoteClipboard(string text)
    {
        _lastRemoteClipboard = text;
        _ignoreClipboard = true;
        try
        {
            if (text.Length == 0) Clipboard.Clear();
            else Clipboard.SetText(text);
        }
        catch
        {
            // 剪贴板正被其他程序占用时跳过这一次
        }
        finally
        {
            _ignoreClipboard = false;
        }
    }

    IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WmClipboardUpdate) ReadLocalClipboard();
        return IntPtr.Zero;
    }

    void ReadLocalClipboard()
    {
        if (_ignoreClipboard || !_connected || _client == null) return;
        string text;
        try
        {
            text = Clipboard.ContainsText() ? Clipboard.GetText() : "";
        }
        catch
        {
            return;
        }
        if (text == _lastRemoteClipboard || text == _lastSentClipboard) return;
        _lastSentClipboard = text;
        _client.SendClipboard(text);
    }

    void UpdateUi()
    {
        var busy = _connecting || _connected;
        ConnectButton.IsEnabled = !busy;
        DisconnectButton.IsEnabled = busy;
        RefreshButton.IsEnabled = _connected;
        OpenButton.IsEnabled = _connected;
        UpButton.IsEnabled = _connected && _remoteRoot.Length > 1;
        UploadButton.IsEnabled = _connected;
        DownloadButton.IsEnabled = _connected;
        DeleteButton.IsEnabled = _connected;
    }

    static int TagInt(ComboBox box, int fallback)
    {
        if (box.SelectedItem is ComboBoxItem item && int.TryParse(item.Tag as string, out var value)) return value;
        return fallback;
    }

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool AddClipboardFormatListener(IntPtr hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool RemoveClipboardFormatListener(IntPtr hwnd);
}

public sealed class FileNode
{
    public string Name { get; set; } = "";
    public string RelativePath { get; set; } = "";
    public bool IsDirectory { get; set; }
    public ulong Size { get; set; }
    public bool IsExpanded { get; set; }
    public ObservableCollection<FileNode> Children { get; } = new();

    public string SizeText =>
        IsDirectory || RelativePath.Length == 0 ? "" : "   " + RemoteFileEntry.FormatSize(Size);
}

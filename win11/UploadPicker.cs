using System.IO;
using System.Runtime.InteropServices;

namespace NetDebug;

static class UploadPicker
{
    const uint ForceFileSystem = 0x40;
    const uint AllowMultiSelect = 0x200;
    const uint PathMustExist = 0x800;
    const uint FileMustExist = 0x1000;
    const uint SigdnFileSysPath = 0x80058000;
    const int Cancelled = unchecked((int)0x800704C7);
    const int EFail = unchecked((int)0x80004005);
    const int GwlpWndProc = -4;
    const uint WmCommand = 0x0111;
    const int IdOk = 1;

    public static string[]? Pick(IntPtr owner)
    {
        var dialog = (IFileOpenDialog)new FileOpenDialog();
        var events = new DialogEvents();
        uint cookie = 0;
        try
        {
            dialog.GetOptions(out var options);
            dialog.SetOptions(options | ForceFileSystem | AllowMultiSelect | PathMustExist | FileMustExist);
            dialog.SetTitle("选择要上传的文件或文件夹");
            dialog.SetOkButtonLabel("上传");
            dialog.Advise(events, out cookie);
            var hr = dialog.Show(owner);
            if (events.Picked != null)
                return events.Picked.Count == 0 ? null : events.Picked.ToArray();
            if (hr == Cancelled || hr < 0) return null;
            dialog.GetResults(out var results);
            return Paths(results);
        }
        catch
        {
            return events.Picked is { Count: > 0 } ? events.Picked.ToArray() : null;
        }
        finally
        {
            events.RemoveHook();
            if (cookie != 0) dialog.Unadvise(cookie);
        }
    }

    static string[] Paths(IShellItemArray array)
    {
        array.GetCount(out var count);
        var paths = new List<string>();
        for (uint i = 0; i < count; i++)
        {
            array.GetItemAt(i, out var item);
            var path = ItemPath(item);
            if (path.Length > 0) paths.Add(path);
        }
        return paths.ToArray();
    }

    static string ItemPath(IShellItem item)
    {
        item.GetDisplayName(SigdnFileSysPath, out var pointer);
        if (pointer == IntPtr.Zero) return "";
        try { return Marshal.PtrToStringUni(pointer) ?? ""; }
        finally { Marshal.FreeCoTaskMem(pointer); }
    }

    static List<string> SelectedPaths(IFileOpenDialog dialog)
    {
        var paths = new List<string>();
        try
        {
            dialog.GetSelectedItems(out var selected);
            selected.GetCount(out var count);
            for (uint i = 0; i < count; i++)
            {
                selected.GetItemAt(i, out var item);
                var path = ItemPath(item);
                if (path.Length > 0 && (File.Exists(path) || Directory.Exists(path))) paths.Add(path);
            }
        }
        catch
        {
            // 对话框还没提交选择时，改看当前高亮项
        }
        if (paths.Count > 0) return paths;
        try
        {
            dialog.GetCurrentSelection(out var current);
            var path = ItemPath(current);
            if (path.Length > 0 && (File.Exists(path) || Directory.Exists(path))) paths.Add(path);
        }
        catch
        {
            // 没有高亮项
        }
        return paths;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    delegate IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    static extern IntPtr SetWindowLongPtr(IntPtr hWnd, int index, IntPtr newProc);

    [DllImport("user32.dll", EntryPoint = "CallWindowProcW")]
    static extern IntPtr CallWindowProc(IntPtr previous, IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [ComImport]
    [Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")]
    class FileOpenDialog
    {
    }

    [ComImport]
    [Guid("00000114-0000-0000-C000-000000000046")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IOleWindow
    {
        void GetWindow(out IntPtr hwnd);
        void ContextSensitiveHelp([MarshalAs(UnmanagedType.Bool)] bool enterMode);
    }

    [ComImport]
    [Guid("d57c7288-d4ad-4768-be02-9d969532d960")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileOpenDialog
    {
        [PreserveSig]
        int Show(IntPtr parent);
        void SetFileTypes(uint count, IntPtr filters);
        void SetFileTypeIndex(uint index);
        void GetFileTypeIndex(out uint index);
        void Advise(IFileDialogEvents events, out uint cookie);
        void Unadvise(uint cookie);
        void SetOptions(uint options);
        void GetOptions(out uint options);
        void SetDefaultFolder(IShellItem item);
        void SetFolder(IShellItem item);
        void GetFolder(out IShellItem item);
        void GetCurrentSelection(out IShellItem item);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string name);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string text);
        void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string label);
        void GetResult(out IShellItem item);
        void AddPlace(IShellItem item, uint placement);
        void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string extension);
        void Close(int result);
        void SetClientGuid(ref Guid guid);
        void ClearClientData();
        void SetFilter(IntPtr filter);
        void GetResults(out IShellItemArray items);
        void GetSelectedItems(out IShellItemArray items);
    }

    [ComImport]
    [Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItem
    {
        void BindToHandler(IntPtr bindCtx, ref Guid handler, ref Guid riid, out IntPtr result);
        void GetParent(out IShellItem parent);
        void GetDisplayName(uint nameType, out IntPtr name);
        void GetAttributes(uint mask, out uint attributes);
        void Compare(IShellItem other, uint hint, out int order);
    }

    [ComImport]
    [Guid("b63ea76d-1f85-456f-a19c-48159efa858b")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellItemArray
    {
        void BindToHandler(IntPtr bindCtx, ref Guid handler, ref Guid riid, out IntPtr result);
        void GetPropertyStore(uint flags, ref Guid riid, out IntPtr store);
        void GetPropertyDescriptionList(IntPtr key, ref Guid riid, out IntPtr list);
        void GetAttributes(int flags, uint mask, out uint attributes);
        void GetCount(out uint count);
        void GetItemAt(uint index, out IShellItem item);
        void EnumItems(out IntPtr enumerator);
    }

    [ComImport]
    [Guid("973510db-7d7f-452b-8975-74a85828d354")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IFileDialogEvents
    {
        [PreserveSig]
        int OnFileOk(IFileOpenDialog dialog);
        [PreserveSig]
        int OnFolderChanging(IFileOpenDialog dialog, IShellItem folder);
        void OnFolderChange(IFileOpenDialog dialog);
        void OnSelectionChange(IFileOpenDialog dialog);
        void OnShareViolation(IFileOpenDialog dialog, IShellItem item, out uint response);
        void OnTypeChange(IFileOpenDialog dialog);
        void OnOverwrite(IFileOpenDialog dialog, IShellItem item, out uint response);
    }

    [ComVisible(true)]
    sealed class DialogEvents : IFileDialogEvents
    {
        public List<string>? Picked;
        readonly WndProc _proc;
        IntPtr _hwnd;
        IntPtr _oldProc;
        bool _confirm;
        bool _closing;

        public DialogEvents() => _proc = Hook;

        public void RemoveHook()
        {
            if (_hwnd == IntPtr.Zero || _oldProc == IntPtr.Zero) return;
            SetWindowLongPtr(_hwnd, GwlpWndProc, _oldProc);
            _hwnd = IntPtr.Zero;
            _oldProc = IntPtr.Zero;
        }

        int IFileDialogEvents.OnFileOk(IFileOpenDialog dialog)
        {
            _confirm = false;
            var picked = SelectedPaths(dialog);
            if (picked.Count > 0) Picked = picked;
            return 0;
        }

        int IFileDialogEvents.OnFolderChanging(IFileOpenDialog dialog, IShellItem folder)
        {
            if (_closing || !_confirm) return 0;
            _confirm = false;
            _closing = true;
            Picked = SelectedPaths(dialog);
            if (Picked.Count == 0)
            {
                var path = ItemPath(folder);
                if (path.Length > 0) Picked.Add(path);
            }
            dialog.Close(0);
            return EFail;
        }

        void IFileDialogEvents.OnFolderChange(IFileOpenDialog dialog) { }

        void IFileDialogEvents.OnSelectionChange(IFileOpenDialog dialog) => InstallHook(dialog);

        void IFileDialogEvents.OnShareViolation(IFileOpenDialog dialog, IShellItem item, out uint response) => response = 0;

        void IFileDialogEvents.OnTypeChange(IFileOpenDialog dialog) { }

        void IFileDialogEvents.OnOverwrite(IFileOpenDialog dialog, IShellItem item, out uint response) => response = 0;

        void InstallHook(IFileOpenDialog dialog)
        {
            if (_hwnd != IntPtr.Zero) return;
            try
            {
                ((IOleWindow)dialog).GetWindow(out _hwnd);
                if (_hwnd == IntPtr.Zero) return;
                _oldProc = SetWindowLongPtr(_hwnd, GwlpWndProc, Marshal.GetFunctionPointerForDelegate(_proc));
            }
            catch
            {
                _hwnd = IntPtr.Zero;
            }
        }

        IntPtr Hook(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
        {
            if (msg == WmCommand && (wParam.ToInt32() & 0xFFFF) == IdOk) _confirm = true;
            return CallWindowProc(_oldProc, hWnd, msg, wParam, lParam);
        }
    }
}

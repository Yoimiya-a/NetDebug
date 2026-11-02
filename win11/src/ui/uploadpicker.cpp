#include "ui/uploadpicker.h"

#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>

#include <QFileInfo>
#include <QWidget>

namespace {

QString itemPath(IShellItem* item) {
    if (!item) return {};
    PWSTR name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) || !name) return {};
    const QString path = QString::fromWCharArray(name);
    CoTaskMemFree(name);
    return path;
}

QStringList existingPaths(IShellItemArray* items) {
    QStringList paths;
    if (!items) return paths;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
        const QString path = itemPath(item);
        item->Release();
        if (!path.isEmpty() && QFileInfo::exists(path)) paths.append(path);
    }
    return paths;
}

constexpr wchar_t kPickerProp[] = L"NetDebugPicker";

QStringList selectedPaths(IFileDialog* dialog) {
    IFileOpenDialog* open = nullptr;
    if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&open)))) return {};
    QStringList paths;
    IShellItemArray* selected = nullptr;
    if (SUCCEEDED(open->GetSelectedItems(&selected)) && selected) {
        paths = existingPaths(selected);
        selected->Release();
    }
    if (paths.isEmpty()) {
        IShellItem* current = nullptr;
        if (SUCCEEDED(open->GetCurrentSelection(&current)) && current) {
            const QString path = itemPath(current);
            current->Release();
            if (!path.isEmpty() && QFileInfo::exists(path)) paths.append(path);
        }
    }
    open->Release();
    return paths;
}

class DialogEvents final : public IFileDialogEvents {
public:
    QStringList picked;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IFileDialogEvents) {
            *object = static_cast<IFileDialogEvents*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --m_refs;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        m_confirm = false;
        const QStringList paths = selectedPaths(dialog);
        if (!paths.isEmpty()) picked = paths;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog* dialog, IShellItem* folder) override {
        if (m_closing || !m_confirm) return S_OK;
        m_confirm = false;
        m_closing = true;
        picked = selectedPaths(dialog);
        if (picked.isEmpty()) {
            const QString path = itemPath(folder);
            if (!path.isEmpty()) picked.append(path);
        }
        dialog->Close(S_OK);
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog* dialog) override {
        installHook(dialog);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (response) *response = FDESVR_DEFAULT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        if (response) *response = FDEOR_DEFAULT;
        return S_OK;
    }

    void removeHook() {
        if (!m_hwnd || !m_old) return;
        SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(m_old));
        RemovePropW(m_hwnd, kPickerProp);
        m_hwnd = nullptr;
        m_old = nullptr;
    }

private:
    ULONG m_refs = 1;
    HWND m_hwnd = nullptr;
    WNDPROC m_old = nullptr;
    bool m_confirm = false;
    bool m_closing = false;

    void installHook(IFileDialog* dialog) {
        if (m_hwnd) return;
        IOleWindow* window = nullptr;
        if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&window)))) return;
        HWND hwnd = nullptr;
        window->GetWindow(&hwnd);
        window->Release();
        if (!hwnd) return;
        SetPropW(hwnd, kPickerProp, this);
        m_hwnd = hwnd;
        m_old = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hook)));
    }

    static LRESULT CALLBACK hook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = static_cast<DialogEvents*>(GetPropW(hwnd, kPickerProp));
        if (self && message == WM_COMMAND && (wparam & 0xFFFF) == IDOK) self->m_confirm = true;
        if (self && self->m_old) return CallWindowProcW(self->m_old, hwnd, message, wparam, lparam);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

}  // namespace

QStringList pickUploads(QWidget* owner) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(created)) {
        if (init == S_OK) CoUninitialize();
        return {};
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    dialog->SetTitle(L"选择要上传的文件或文件夹");
    dialog->SetOkButtonLabel(L"上传");

    auto* events = new DialogEvents;
    DWORD cookie = 0;
    dialog->Advise(events, &cookie);
    const HRESULT shown = dialog->Show(owner ? reinterpret_cast<HWND>(owner->winId()) : nullptr);
    QStringList result = events->picked;
    if (result.isEmpty() && SUCCEEDED(shown)) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items)) && items) {
            result = existingPaths(items);
            items->Release();
        }
    }
    events->removeHook();
    if (cookie) dialog->Unadvise(cookie);
    events->Release();
    dialog->Release();
    if (init == S_OK) CoUninitialize();
    if (FAILED(shown) && result.isEmpty()) return {};
    return result;
}

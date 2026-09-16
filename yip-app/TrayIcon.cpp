#include "pch.h"
#include "TrayIcon.h"

#include "resource.h"

#include <windowsx.h>

namespace {

constexpr wchar_t kHostClass[] = L"YipTrayHostWindow";

// Private callback message. WM_APP+n is the range reserved for exactly this.
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kIconId = 1;
constexpr UINT_PTR kRetryTimer = 1;

constexpr UINT kCmdToggleRecording = 0x100;
constexpr UINT kCmdShow = 0x101;
constexpr UINT kCmdExit = 0x102;

// NIM_ADD fails while the shell is still coming up, which is exactly the case
// when Yip is launched at sign-in. Giving up on the first failure is the other
// usual reason a tray icon never appears, so failures are retried for ~20s.
constexpr int kMaxAddAttempts = 20;
constexpr UINT kRetryIntervalMs = 1000;

constexpr wchar_t kTipIdle[] = L"Yip — idle";
constexpr wchar_t kTipRecording[] = L"Yip — recording";

// Broadcast by the shell when the taskbar is (re)created: explorer restarting
// takes every notification icon with it and expects each owner to re-add.
UINT TaskbarCreatedMessage()
{
    static const UINT message = ::RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

// LR_SHARED hands back a cached handle that must not be destroyed. The size is
// the shell's small-icon metric so the .ico frame drawn for it is picked rather
// than a scaled larger one.
HICON LoadTrayIcon(int resourceId)
{
    return static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resourceId),
                                           IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON),
                                           ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
}

} // namespace

namespace yip {

const wchar_t* TrayIcon::HostClassName() noexcept
{
    return kHostClass;
}

UINT TrayIcon::ActivateMessage()
{
    static const UINT message = ::RegisterWindowMessageW(L"YipActivateInstance");
    return message;
}

TrayIcon::TrayIcon(Callbacks callbacks) : m_callbacks(std::move(callbacks))
{
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    static const ATOM atom = [instance] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TrayIcon::WndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kHostClass;
        return ::RegisterClassExW(&wc);
    }();
    if (atom == 0) return;

    m_iconIdle = LoadTrayIcon(IDI_YIP_TRAY_IDLE);
    m_iconRecording = LoadTrayIcon(IDI_YIP_TRAY_RECORDING);
    // A slot with no glyph in it is worse than the tile at 16px.
    if (!m_iconIdle) m_iconIdle = LoadTrayIcon(IDI_YIP_APP);
    if (!m_iconRecording) m_iconRecording = m_iconIdle;

    // Hidden, but a real top-level window: WS_EX_TOOLWINDOW keeps it out of the
    // taskbar and Alt-Tab, and never being shown keeps it off the screen.
    m_host = ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Yip", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                               instance, this);
    if (m_host) AddIcon();
}

TrayIcon::~TrayIcon()
{
    RemoveIcon();
    if (m_host) {
        ::KillTimer(m_host, kRetryTimer);
        ::DestroyWindow(m_host);
        m_host = nullptr;
    }
    // m_iconIdle / m_iconRecording are LR_SHARED — not ours to destroy.
}

HICON TrayIcon::CurrentIcon() const noexcept
{
    return m_recording ? m_iconRecording : m_iconIdle;
}

const wchar_t* TrayIcon::CurrentTip() const noexcept
{
    return m_recording ? kTipRecording : kTipIdle;
}

void TrayIcon::SetRecording(bool recording)
{
    if (m_recording == recording) return;
    m_recording = recording;
    ApplyIconState();
}

bool TrayIcon::AddIcon()
{
    if (m_added || !m_host) return m_added;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_host;
    nid.uID = kIconId;
    // No guidItem on purpose: a GUID binds the icon to the exe's path, and after
    // the exe moves the shell refuses the registration and shows nothing.
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayCallback;
    nid.hIcon = CurrentIcon();
    ::wcscpy_s(nid.szTip, CurrentTip());

    if (!::Shell_NotifyIconW(NIM_ADD, &nid)) {
        if (++m_addAttempts < kMaxAddAttempts) {
            ::SetTimer(m_host, kRetryTimer, kRetryIntervalMs, nullptr);
        }
        return false;
    }

    // Version 4 moves the click coordinates into wParam and the event id into
    // the low word of lParam, and is what NIF_SHOWTIP is defined against.
    nid.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &nid);

    m_added = true;
    m_addAttempts = 0;
    return true;
}

void TrayIcon::ShowHint(const wchar_t* title, const wchar_t* body)
{
    if (!m_added || !m_host) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_host;
    nid.uID = kIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;  // a window closing is not an event worth a chime
    ::wcscpy_s(nid.szInfoTitle, title);
    ::wcscpy_s(nid.szInfo, body);
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::RemoveIcon()
{
    if (!m_added || !m_host) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_host;
    nid.uID = kIconId;
    ::Shell_NotifyIconW(NIM_DELETE, &nid);
    m_added = false;
}

void TrayIcon::ApplyIconState()
{
    if (!m_added || !m_host) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_host;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = CurrentIcon();
    ::wcscpy_s(nid.szTip, CurrentTip());
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowMenu(POINT anchor)
{
    const HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    ::AppendMenuW(menu, MF_STRING, kCmdToggleRecording,
                  m_recording ? L"Stop recording" : L"Start recording");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCmdShow, L"Open Yip");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

    // A tray menu only dismisses on an outside click while its owner window is
    // foreground, and the trailing WM_NULL is the documented fix for the click
    // that would otherwise be swallowed afterwards.
    ::SetForegroundWindow(m_host);

    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
    flags |= (::GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command =
        static_cast<UINT>(::TrackPopupMenuEx(menu, flags, anchor.x, anchor.y, m_host, nullptr));

    ::DestroyMenu(menu);
    ::PostMessageW(m_host, WM_NULL, 0, 0);

    switch (command) {
        case kCmdToggleRecording:
            if (m_callbacks.onToggleRecording) m_callbacks.onToggleRecording();
            break;
        case kCmdShow:
            if (m_callbacks.onShow) m_callbacks.onShow();
            break;
        case kCmdExit:
            if (m_callbacks.onExit) m_callbacks.onExit();
            break;
        default:
            break;
    }
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (auto* self = reinterpret_cast<TrayIcon*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return self->Handle(hwnd, msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayIcon::Handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == TaskbarCreatedMessage()) {
        // The old registration died with the old taskbar.
        m_added = false;
        m_addAttempts = 0;
        AddIcon();
        return 0;
    }
    if (msg == ActivateMessage()) {
        if (m_callbacks.onShow) m_callbacks.onShow();
        return 0;
    }

    switch (msg) {
        case kTrayCallback: {
            // NOTIFYICON_VERSION_4 packing: event id in the low word of lParam,
            // anchor point (screen coordinates, signed for multi-monitor) in
            // wParam.
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (m_callbacks.onShow) m_callbacks.onShow();
                    return 0;
                case WM_CONTEXTMENU: {
                    const POINT anchor{GET_X_LPARAM(static_cast<LPARAM>(wParam)),
                                       GET_Y_LPARAM(static_cast<LPARAM>(wParam))};
                    ShowMenu(anchor);
                    return 0;
                }
                default:
                    return 0;
            }
        }
        case WM_TIMER:
            if (wParam == kRetryTimer) {
                ::KillTimer(hwnd, kRetryTimer);
                AddIcon();
                return 0;
            }
            break;
        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            m_host = nullptr;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace yip

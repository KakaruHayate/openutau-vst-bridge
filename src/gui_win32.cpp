/*
 * The info window's Win32 backend: a small fixed-size window that redraws the session's
 * UiState a few times a second. Everything here runs on the main thread — the CLAP gui
 * extension's own requirement — and the only cross-thread touch is UiCopy(), which is
 * built for exactly that.
 *
 * The extension's callbacks receive the clap_plugin_t, not the window, so instances live
 * in a registry keyed by the plugin pointer, filled by plugin.cpp around init/destroy.
 * Drawing is deliberately plain: system colors and the stock GUI font, which are what keep
 * the window legible on any theme without a custom look to maintain.
 */

#include "gui.h"

#include "session.h"

#ifndef UNICODE
#define UNICODE  // IDC_ARROW and every W call below assume it.
#endif
#ifndef NOMINMAX
#define NOMINMAX  // std::max below, not windows.h's macros.
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bridge {

namespace guiwin {

constexpr wchar_t kClassName[] = L"OpenUtauBridgeInfo";
constexpr uint32_t kWindowWidth = 320;
constexpr uint32_t kWindowHeight = 220;
constexpr UINT_PTR kTimerId = 1;
constexpr int kTimerPeriodMs = 250;
constexpr int kLineHeight = 18;
constexpr int kTrackRows = 6;
constexpr DWORD kFloatingStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;

std::wstring Utf16(const std::string &utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                   nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(),
                        size);
    return wide;
}

/// One painted line, laid out top to bottom.
struct Row {
    std::wstring text;
    COLORREF color = 0;
    int indent = 0;
};

/// Everything the window is, keyed to the hwnd through GWLP_USERDATA. Not thread-safe: the
/// gui extension guarantees main-thread calls, and the timer fires there too.
struct WindowState {
    Session *session = nullptr;
    HWND hwnd = nullptr;
    bool floating = true;
    UiState shown;  // What the last paint drew, so an unchanged state skips InvalidateRect.

    std::vector<Row> Rows() const {
        std::vector<Row> rows;
        COLORREF strong = GetSysColor(COLOR_WINDOWTEXT);
        COLORREF weak = GetSysColor(COLOR_GRAYTEXT);
        COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);

        Row title;
        title.text = L"OpenUtau Bridge";
        title.color = strong;
        rows.push_back(title);

        Row connection;
        connection.text = shown.connected
            ? L"Connected on port " + std::to_wstring(shown.port)
            : L"Not connected";
        connection.color = shown.connected ? accent : weak;
        rows.push_back(connection);

        Row project;
        if (shown.projectSaved && !shown.projectName.empty()) {
            project.text = L"Project: " + Utf16(shown.projectName);
        } else {
            project.text = L"Project: (unsaved)";
        }
        project.color = strong;
        rows.push_back(project);

        Row tempo;
        tempo.text = shown.hasTempo
            ? L"Tempo: " + std::to_wstring(static_cast<int>(shown.tempo + 0.5)) + L" BPM"
            : L"Tempo: (unknown)";
        tempo.color = strong;
        rows.push_back(tempo);

        Row transport;
        transport.text = shown.playing ? L"Transport: playing" : L"Transport: stopped";
        transport.color = shown.playing ? accent : strong;
        rows.push_back(transport);

        // The tracks, with this instance's own marked. More than fit are simply not shown:
        // the window is a glance, not a mixer.
        for (int y = 0; y < kTrackRows && y < static_cast<int>(shown.trackNames.size()); y++) {
            Row track;
            track.text = (y == shown.trackNo ? L"\x25B8 " : L"  ") +
                         std::to_wstring(y + 1) + L": " + Utf16(shown.trackNames[y]);
            track.color = y == shown.trackNo ? accent : strong;
            track.indent = 12;
            rows.push_back(track);
        }
        if (shown.trackNames.empty()) {
            Row none;
            none.text = L"No tracks reported yet.";
            none.color = weak;
            none.indent = 12;
            rows.push_back(none);
        }
        return rows;
    }

    void Paint(HDC dc, const RECT &client) {
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));

        RECT row{client.left + 12, client.top + 10, client.right - 12,
                 client.top + 10 + kLineHeight};
        for (const Row &line : Rows()) {
            if (row.top >= client.bottom) {
                break;
            }
            RECT cell = row;
            cell.left += line.indent;
            SetTextColor(dc, line.color);
            DrawTextW(dc, line.text.c_str(), -1, &cell, DT_SINGLELINE | DT_END_ELLIPSIS);
            row.top += kLineHeight;
            row.bottom += kLineHeight;
        }
        SelectObject(dc, font);
    }

    void OnTimer() {
        UiState current = session->UiCopy();
        bool changed = current.connected != shown.connected || current.port != shown.port ||
                       current.projectName != shown.projectName ||
                       current.projectSaved != shown.projectSaved ||
                       current.trackNames != shown.trackNames ||
                       current.trackNo != shown.trackNo || current.hasTempo != shown.hasTempo ||
                       current.playing != shown.playing ||
                       (current.hasTempo &&
                        static_cast<int>(current.tempo) != static_cast<int>(shown.tempo));
        if (changed) {
            shown = current;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    void ApplyStyle(bool asFloating) {
        floating = asFloating;
        SetWindowLongPtrW(hwnd, GWL_STYLE,
                          asFloating ? kFloatingStyle : (WS_CHILD | WS_VISIBLE));
    }
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *state = reinterpret_cast<WindowState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client;
            GetClientRect(hwnd, &client);
            if (state != nullptr) {
                state->Paint(dc, client);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;  // Paint fills the whole client area; erasing too would flicker.
        case WM_TIMER:
            if (state != nullptr && wparam == kTimerId) {
                state->OnTimer();
            }
            return 0;
        case WM_CLOSE:
            // A floating window closed by its user hides rather than destroys: the host owns
            // the gui's lifetime, and it was not asked for destroy().
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

bool EnsureWindowClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSEXW klass = {};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = WndProc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = nullptr;
    klass.lpszClassName = kClassName;
    registered = RegisterClassExW(&klass) != 0;
    return registered;
}

}  // namespace guiwin

// --------------------------------------------------------------------- the registry

namespace {

std::mutex &RegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<const clap_plugin_t *, InfoWindow *> &Registry() {
    static std::map<const clap_plugin_t *, InfoWindow *> registry;
    return registry;
}

/// The window whose plugin called in, or null — every gui callback starts here.
InfoWindow *Instance(const clap_plugin_t *plugin) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    auto it = Registry().find(plugin);
    return it != Registry().end() ? it->second : nullptr;
}

bool Supported(const char *api, bool isFloating) {
    if (isFloating) {
        // Floating windows may be asked for with a null or blank api.
        return api == nullptr || api[0] == '\0' ||
               std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
    }
    return api != nullptr && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
}

bool IsWin32Window(const clap_window_t *window) {
    return window != nullptr && window->api != nullptr &&
           std::strcmp(window->api, CLAP_WINDOW_API_WIN32) == 0;
}

const clap_plugin_gui_t &GuiTable() {
    static const clap_plugin_gui_t kGui = {
        // is_api_supported
        +[](const clap_plugin_t *, const char *api, bool isFloating) {
            return Supported(api, isFloating);
        },
        // get_preferred_api
        +[](const clap_plugin_t *, const char **api, bool *isFloating) {
            *api = CLAP_WINDOW_API_WIN32;
            // Floating, so the window behaves the same in every host that shows one at all.
            *isFloating = true;
            return true;
        },
        // create — the window already exists per instance; a host that embeds re-parents it
        // in set_parent below.
        +[](const clap_plugin_t *plugin, const char *, bool) {
            return Instance(plugin) != nullptr;
        },
        // destroy — only hides; the window itself dies when the plugin is destroyed.
        +[](const clap_plugin_t *plugin) {
            InfoWindow *info = Instance(plugin);
            if (info != nullptr) {
                info->Hide();
            }
        },
        // set_scale — physical pixels are used as-is, so a scale factor is not applied and
        // is reported as ignored rather than silently accepted.
        +[](const clap_plugin_t *, double) { return false; },
        // get_size
        +[](const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
            if (Instance(plugin) == nullptr) {
                return false;
            }
            *width = guiwin::kWindowWidth;
            *height = guiwin::kWindowHeight;
            return true;
        },
        // can_resize
        +[](const clap_plugin_t *) { return false; },
        // get_resize_hints
        +[](const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints) {
            if (Instance(plugin) == nullptr) {
                return false;
            }
            hints->can_resize_horizontally = false;
            hints->can_resize_vertically = false;
            hints->preserve_aspect_ratio = false;
            return true;
        },
        // adjust_size — the size is fixed, so the adjustment is the size itself.
        +[](const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
            if (Instance(plugin) == nullptr) {
                return false;
            }
            *width = guiwin::kWindowWidth;
            *height = guiwin::kWindowHeight;
            return true;
        },
        // set_size — accepted so a host restoring a session is not told no, and then
        // overridden: the window draws itself at its fixed size regardless.
        +[](const clap_plugin_t *, uint32_t, uint32_t) { return true; },
        // set_parent
        +[](const clap_plugin_t *plugin, const clap_window_t *window) {
            InfoWindow *info = Instance(plugin);
            return info != nullptr && IsWin32Window(window) &&
                   (info->EmbedInto(window->win32), true);
        },
        // set_transient
        +[](const clap_plugin_t *plugin, const clap_window_t *window) {
            InfoWindow *info = Instance(plugin);
            return info != nullptr && IsWin32Window(window) &&
                   (info->OwnTo(window->win32), true);
        },
        // suggest_title
        +[](const clap_plugin_t *plugin, const char *title) {
            InfoWindow *info = Instance(plugin);
            if (info != nullptr && title != nullptr) {
                info->Retitle(title);
            }
        },
        // show
        +[](const clap_plugin_t *plugin) {
            InfoWindow *info = Instance(plugin);
            if (info == nullptr) {
                return false;
            }
            info->Show();
            return true;
        },
        // hide
        +[](const clap_plugin_t *plugin) {
            InfoWindow *info = Instance(plugin);
            if (info == nullptr) {
                return false;
            }
            info->Hide();
            return true;
        },
    };
    return kGui;
}

}  // namespace

void GuiRegister(const clap_plugin_t *plugin, InfoWindow *window) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Registry()[plugin] = window;
}

void GuiUnregister(const clap_plugin_t *plugin) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Registry().erase(plugin);
}

InfoWindow *CreateInfoWindow(Session *session) {
    using guiwin::WindowState;
    if (!guiwin::EnsureWindowClass()) {
        return nullptr;
    }
    auto *window = new InfoWindow();
    auto *state = new WindowState();
    state->session = session;
    state->shown = session->UiCopy();
    state->hwnd = CreateWindowExW(
        0, guiwin::kClassName, L"OpenUtau Bridge", guiwin::kFloatingStyle, CW_USEDEFAULT,
        CW_USEDEFAULT, static_cast<int>(guiwin::kWindowWidth),
        static_cast<int>(guiwin::kWindowHeight), nullptr, nullptr, GetModuleHandleW(nullptr),
        state);
    if (state->hwnd == nullptr) {
        delete state;
        delete window;
        return nullptr;
    }
    window->impl_ = state;
    return window;
}

InfoWindow::~InfoWindow() {
    auto *state = static_cast<guiwin::WindowState *>(impl_);
    if (state == nullptr) {
        return;
    }
    if (state->hwnd != nullptr) {
        KillTimer(state->hwnd, guiwin::kTimerId);
        DestroyWindow(state->hwnd);
    }
    delete state;
}

const clap_plugin_gui_t *InfoWindow::Extension() const { return &GuiTable(); }

void InfoWindow::Show() {
    auto *state = static_cast<guiwin::WindowState *>(impl_);
    if (state->floating) {
        // Centered over its owner each time: the host may have moved since the last show.
        RECT area{};
        HWND owner = GetWindow(state->hwnd, GW_OWNER);
        if (GetWindowRect(owner != nullptr ? owner : state->hwnd, &area)) {
            int width = static_cast<int>(guiwin::kWindowWidth);
            int height = static_cast<int>(guiwin::kWindowHeight);
            int x = area.left + std::max(0, (static_cast<int>(area.right) -
                                             static_cast<int>(area.left) - width) / 2);
            int y = area.top + std::max(0, (static_cast<int>(area.bottom) -
                                            static_cast<int>(area.top) - height) / 2);
            SetWindowPos(state->hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    state->shown = state->session->UiCopy();
    InvalidateRect(state->hwnd, nullptr, FALSE);
    SetTimer(state->hwnd, guiwin::kTimerId, guiwin::kTimerPeriodMs, nullptr);
    ShowWindow(state->hwnd, SW_SHOWNA);
}

void InfoWindow::Hide() {
    auto *state = static_cast<guiwin::WindowState *>(impl_);
    KillTimer(state->hwnd, guiwin::kTimerId);
    ShowWindow(state->hwnd, SW_HIDE);
}

void InfoWindow::EmbedInto(void *parent) {
    auto *state = static_cast<guiwin::WindowState *>(impl_);
    state->ApplyStyle(false);
    SetParent(state->hwnd, static_cast<HWND>(parent));
}

void InfoWindow::OwnTo(void *owner) {
    auto *state = static_cast<guiwin::WindowState *>(impl_);
    SetWindowLongPtrW(state->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
}

void InfoWindow::Retitle(const char *title) {
    auto *state = static_cast<guiwin::WindowState *>(impl_);
    SetWindowTextW(state->hwnd, guiwin::Utf16(title).c_str());
}

}  // namespace bridge

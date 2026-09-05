/*
 * The info window's Win32 backend: a small fixed-size window holding the session's
 * UiState — plain painted text rows for the info, and one real dropdown-list combobox
 * for the track picker. Everything here runs on the main thread — the CLAP gui
 * extension's own requirement — and the only cross-thread touch is UiCopy(), which is
 * built for exactly that.
 *
 * The extension's callbacks receive the clap_plugin_t, not the window, so instances live
 * in a registry keyed by the plugin pointer, filled by plugin.cpp around init/destroy.
 * The look is deliberately plain: system colors and the stock GUI font, which are what
 * keep the window legible on any theme without a custom look to maintain.
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
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bridge {

namespace guiwin {

constexpr wchar_t kClassName[] = L"OpenUtauBridgeInfo";
constexpr uint32_t kWindowWidth = 320;
constexpr uint32_t kWindowHeight = 196;
constexpr UINT_PTR kTimerId = 1;
constexpr int kTimerPeriodMs = 250;
constexpr int kLineHeight = 18;
constexpr int kComboId = 1001;
// Painted rows end here; the combobox sits just below, with a matching bottom margin.
constexpr int kComboY = 10 + 6 * kLineHeight + 8;
constexpr int kComboHeight = 132;  // Closed box plus roughly five visible list entries.
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

/// The dropdown item for a track: "N: name - singer - engine", with the informational
/// fields simply left out when OpenUtau reports none.
std::wstring TrackLabel(const TrackInfo &track, int index) {
    std::wstring label = std::to_wstring(index + 1) + L": " + Utf16(track.name);
    if (!track.singer.empty()) {
        label += L"  \x2014  " + Utf16(track.singer);
    }
    if (!track.engine.empty()) {
        label += L"  \x00B7  " + Utf16(track.engine);
    }
    return label;
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
    std::function<void()> onTrackPicked;
    HWND hwnd = nullptr;
    HWND combo = nullptr;
    bool floating = true;
    UiState shown;  // What the last paint drew, so an unchanged state skips repaints.
    std::vector<std::wstring> comboLabels;  // What the dropdown currently lists.

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

        // The routed track's singer and engine, informational only — the dropdown itself
        // already shows the track names.
        Row singer;
        singer.indent = 12;
        if (shown.trackNo >= 0 && shown.trackNo < static_cast<int>(shown.tracks.size())) {
            const TrackInfo &track = shown.tracks[static_cast<size_t>(shown.trackNo)];
            std::wstring who = track.singer.empty() ? L"(none)" : Utf16(track.singer);
            std::wstring engine = track.engine.empty() ? L"(none)" : Utf16(track.engine);
            singer.text = L"Track " + std::to_wstring(shown.trackNo + 1) +
                          L" \x2014 singer: " + who + L" \x00B7 engine: " + engine;
            singer.color = strong;
        } else {
            singer.text = L"No tracks reported yet.";
            singer.color = weak;
        }
        rows.push_back(singer);
        return rows;
    }
};

namespace {

void Paint(WindowState *state, HDC dc, const RECT &client) {
    FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));

    RECT row{client.left + 12, client.top + 10, client.right - 12,
             client.top + 10 + kLineHeight};
    for (const Row &line : state->Rows()) {
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

/// Rebuilds the dropdown when OpenUtau's track list changed, and follows the routed
/// track's selection. CB_SETCURSEL does not fire CBN_SELCHANGE, so the programmatic
/// follow cannot echo back as a user pick.
void SyncTracks(WindowState *state) {
    std::vector<std::wstring> labels;
    labels.reserve(state->shown.tracks.size());
    for (size_t i = 0; i < state->shown.tracks.size(); i++) {
        labels.push_back(TrackLabel(state->shown.tracks[i], static_cast<int>(i)));
    }

    if (labels != state->comboLabels) {
        SendMessageW(state->combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(state->combo, CB_RESETCONTENT, 0, 0);
        for (const std::wstring &label : labels) {
            SendMessageW(state->combo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(state->combo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(state->combo, nullptr, TRUE);
        state->comboLabels = std::move(labels);
    }

    int count = static_cast<int>(state->shown.tracks.size());
    if (count > 0 && state->shown.trackNo >= 0 && state->shown.trackNo < count) {
        if (SendMessageW(state->combo, CB_GETCURSEL, 0, 0) != state->shown.trackNo) {
            SendMessageW(state->combo, CB_SETCURSEL, state->shown.trackNo, 0);
        }
    } else {
        SendMessageW(state->combo, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
}

void OnTimer(WindowState *state) {
    // Unconditional: copy, sync and repaint at 4 Hz. The change-skipping optimization of
    // the 1.1 window is gone — Windows may discard the redraw surface of an occluded or
    // minimized window, and a window that only invalidates on change then comes back
    // showing garbage (the "black after connecting" reports against 1.1). A 4 Hz repaint
    // of a 320-point window costs nothing and self-heals every such case.
    state->shown = state->session->UiCopy();
    SyncTracks(state);
    InvalidateRect(state->hwnd, nullptr, FALSE);
}

void ApplyStyle(WindowState *state, bool asFloating) {
    state->floating = asFloating;
    SetWindowLongPtrW(state->hwnd, GWL_STYLE,
                      asFloating ? kFloatingStyle : (WS_CHILD | WS_VISIBLE));
}

void PlaceCombo(WindowState *state) {
    RECT client;
    GetClientRect(state->hwnd, &client);
    int width = static_cast<int>(client.right) - 24;
    if (width > 0) {
        MoveWindow(state->combo, 12, kComboY, width, kComboHeight, TRUE);
    }
}

}  // namespace

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
                Paint(state, dc, client);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_PRINTCLIENT:
            // Hosts may ask the window to draw itself into a DC of their choosing (for
            // caching or thumbnails). Not answering is how a window turns up black
            // inside its host.
            if (state != nullptr) {
                RECT client;
                GetClientRect(hwnd, &client);
                Paint(state, reinterpret_cast<HDC>(wparam), client);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;  // Paint fills the whole client area; erasing too would flicker.
        case WM_TIMER:
            if (state != nullptr && wparam == kTimerId) {
                OnTimer(state);
            }
            return 0;
        case WM_SIZE:
            if (state != nullptr && state->combo != nullptr) {
                PlaceCombo(state);
            }
            return 0;
        case WM_COMMAND:
            // Only the user's pick lands here — CB_SETCURSEL from SyncTracks does not
            // raise CBN_SELCHANGE, so host-side track changes cannot echo into a request.
            if (state != nullptr && LOWORD(wparam) == kComboId &&
                HIWORD(wparam) == CBN_SELCHANGE) {
                int selection = static_cast<int>(
                    SendMessageW(state->combo, CB_GETCURSEL, 0, 0));
                if (selection >= 0) {
                    state->session->RequestTrackNo(selection);
                    if (state->onTrackPicked) {
                        state->onTrackPicked();
                    }
                }
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
    // A real background brush: any region the system erases outside our paint (expose,
    // resize, DWM discard) must come out as COLOR_WINDOW, never as uninitialized black.
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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

InfoWindow *CreateInfoWindow(Session *session, std::function<void()> onTrackPicked) {
    using guiwin::WindowState;
    if (!guiwin::EnsureWindowClass()) {
        return nullptr;
    }
    auto *window = new InfoWindow();
    auto *state = new WindowState();
    state->session = session;
    state->onTrackPicked = std::move(onTrackPicked);
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
    state->combo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 12,
        guiwin::kComboY, static_cast<int>(guiwin::kWindowWidth) - 24, guiwin::kComboHeight,
        state->hwnd, reinterpret_cast<HMENU>(static_cast<LONG_PTR>(guiwin::kComboId)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(state->combo, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    guiwin::SyncTracks(state);
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
    guiwin::SyncTracks(state);
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
    guiwin::ApplyStyle(state, false);
    SetParent(state->hwnd, static_cast<HWND>(parent));
    guiwin::PlaceCombo(state);
}

void InfoWindow::OwnTo(void *owner) {
    auto *state = static_cast<guiwin::WindowState *>(impl_);
    SetWindowLongPtrW(state->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
}

void InfoWindow::Retitle(const char *title) {
    auto *state = static_cast<guiwin::WindowState *>(impl_);
    SetWindowTextW(state->hwnd, guiwin::Utf16(title).c_str());
}

bool InfoWindow::SetContentScale(float) {
    return false;  // Physical pixels throughout; see the gui table's set_scale.
}

}  // namespace bridge

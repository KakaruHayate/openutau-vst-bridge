/*
 * The info window's X11 backend: one plain Xlib window holding the session's UiState —
 * painted Xft text rows for the info, and a hand-rolled dropdown for the track picker.
 * X11 has no combobox control to lean on (that is Motif's, and nobody links Motif in
 * 2026), so the picker is a second override-redirect window with a pointer grab — the
 * way menus themselves have always worked. Xft renders the UTF-8 track names that plain
 * XDrawString cannot. Like the other backends there is no toolkit dependency: a window
 * of five labels and one dropdown is not worth a UI framework's build time, binary size
 * or crash surface.
 *
 * Same contract as gui_win32.cpp: UiCopy() as the sole cross-thread touch, instances
 * registered by plugin.cpp and looked up per gui callback. One deliberate deviation: X
 * has no per-window timer and an X event queue nobody drains is a window that never
 * repaints, so a small event thread of our own drains our own display connection —
 * Xlib is serialized behind XInitThreads, UiCopy/RequestTrackNo are thread-safe, and
 * the tick repaints at 4 Hz exactly like the Win32 timer does. The one cost: the
 * onTrackPicked nudge (host request_flush) fires from that thread rather than the
 * host's main thread. Every X11 host that embeds plugins already tolerates a flush
 * request from a plugin's own event source; a strictly main-thread host would simply
 * see the parameter change on its next flush regardless.
 *
 * Untested against a real host — the backend was written to the CLAP contract and the
 * two working backends, and its first compile is CI's. Expect rough edges.
 */

#include "gui.h"

#include "session.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xft/Xft.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <sys/select.h>
#include <thread>
#include <vector>

namespace bridge {
namespace x11gui {

constexpr uint32_t kWindowWidth = 320;
constexpr uint32_t kWindowHeight = 180;
constexpr int kMargin = 12;
constexpr int kTop = 10;
constexpr int kLineHeight = 18;
// Painted rows end here (seven of them); the combobox sits just below, with a matching
// bottom margin — the same layout as the Win32 backend.
constexpr int kComboY = kTop + 7 * kLineHeight + 8;
constexpr int kComboHeight = 20;
constexpr int kComboItemHeight = 20;
constexpr int kPopupMaxItems = 10;
constexpr long kTickMs = 250;  // One 4 Hz repaint, like the other backends' timer.

// The dark palette, the same values the Win32 backend paints: hosts are overwhelmingly
// dark-windowed DAWs, and following the system theme would hand us a glaring white
// panel inside them.
struct Rgb {
    unsigned char r, g, b;
};
constexpr Rgb kBackground{32, 32, 32};
constexpr Rgb kListBackground{24, 24, 24};
constexpr Rgb kSelection{55, 61, 69};
constexpr Rgb kBorder{88, 88, 88};
constexpr Rgb kTextStrong{235, 235, 235};
constexpr Rgb kTextDim{165, 165, 165};
constexpr Rgb kAccent{76, 194, 255};  // Fluent dark accent #4CC2FF, ~8:1 on the bg.

/// The dropdown item for a track: "N: name". Names only — the list has to stay
/// scannable; the singer and engine of the routed track live in the info rows above.
std::string TrackLabel(const TrackInfo &track, size_t index) {
    return std::to_string(index + 1) + ": " + track.name;
}

/// One painted line, laid out top to bottom.
struct Row {
    std::string text;
    const XftColor *color = nullptr;
    int indent = 0;
};

/// Everything the window is. The gui extension guarantees main-thread calls, but the
/// event thread below runs the repaints and the input, so the fields it shares with the
/// main thread are either atomic or guarded by popupMutex; Xlib calls themselves are
/// serialized by XInitThreads.
struct WindowState {
    Session *session = nullptr;
    std::function<void()> onTrackPicked;
    Display *display = nullptr;
    int screen = 0;
    Colormap colormap = 0;
    Window window = 0;  // The main window: info rows plus the closed combobox.
    Window popup = 0;   // The dropped list, override-redirect, parented to the root.
    GC gc = nullptr;
    XftDraw *draw = nullptr;
    XftDraw *popupDraw = nullptr;
    XftFont *font = nullptr;
    XftColor background, listBackground, selection, border, textStrong, textDim, accent;
    UiState shown;  // What the last paint drew, so an unchanged state costs one copy.
    std::vector<std::string> comboLabels;  // What the dropdown currently lists.
    std::string title = "OpenUtau Bridge";
    Atom deleteAtom = 0;  // WM_DELETE_WINDOW: the floating window's close box.

    // The event thread below: started once the window exists, joined at destruction.
    // Atomic because the destructor stops it from the main thread; parent/owner/floating
    // are atomic for the same reason — the main thread writes them in EmbedInto/OwnTo
    // while the event thread reads and clears them in HideWindow (the floating window's
    // close box can race the host's hide()).
    std::atomic<bool> running{false};
    std::thread eventThread;
    std::atomic<Window> parent{0};  // The host window we are embedded into, if any.
    std::atomic<Window> owner{0};   // The host window we are transient to, if any.
    std::atomic<bool> floating{true};

    // The popup's world. popupMutex guards popupOpen and the popup's map/unmap
    // transitions, because Hide() arrives on the main thread while the event thread may
    // be mid-browse; popupScroll and the popup geometry are event-thread only, but they
    // share the lock for simplicity.
    std::mutex popupMutex;
    bool popupOpen = false;
    int popupScroll = 0;
    int popupWidth = 0;
    int popupHeight = 0;

    bool alive() const { return display != nullptr && window != 0; }

    std::vector<Row> Rows() const {
        std::vector<Row> rows;
        const XftColor *strong = &textStrong;
        const XftColor *weak = &textDim;
        const XftColor *hot = &accent;

        rows.push_back({"OpenUtau Bridge", strong, 0});

        rows.push_back({shown.connected ? "Connected on port " + std::to_string(shown.port)
                                        : std::string("Not connected"),
                        shown.connected ? hot : weak, 0});

        if (shown.projectSaved && !shown.projectName.empty()) {
            rows.push_back({"Project: " + shown.projectName, strong, 0});
        } else {
            rows.push_back({"Project: (unsaved)", strong, 0});
        }

        rows.push_back({shown.hasTempo ? "Tempo: " +
                                             std::to_string(static_cast<int>(shown.tempo + 0.5)) +
                                             " BPM"
                                       : std::string("Tempo: (unknown)"),
                        strong, 0});

        rows.push_back({shown.playing ? "Transport: playing" : "Transport: stopped",
                        shown.playing ? hot : strong, 0});

        // The routed track's singer and engine, each on its own row: one long line
        // truncates as soon as both names are reasonably long.
        if (shown.trackNo >= 0 && shown.trackNo < static_cast<int>(shown.tracks.size())) {
            const TrackInfo &track = shown.tracks[static_cast<size_t>(shown.trackNo)];
            rows.push_back({"Singer: " + (track.singer.empty() ? "(none)" : track.singer),
                            strong, 12});
            rows.push_back({"Engine: " + (track.engine.empty() ? "(none)" : track.engine),
                            strong, 12});
        } else {
            rows.push_back({"No tracks reported yet.", weak, 12});
            rows.push_back({"", weak, 12});
        }
        return rows;
    }
};

namespace {

/// The vertical center of one 18 px row, as an Xft baseline: half a line down, then
/// corrected by half the font's ascent-descent imbalance.
int Baseline(XftFont *font, int rowTop) {
    return rowTop + kLineHeight / 2 + (font->ascent - font->descent) / 2;
}

/// Clips one line to maxWidth with a trailing ellipsis — the DT_END_ELLIPSIS of the
/// Win32 backend. Xft draws whatever it is handed, so the clipping happens here.
std::string Ellipsize(Display *display, XftFont *font, const std::string &text,
                      int maxWidth) {
    XGlyphInfo extents;
    const auto measure = [&](const std::string &candidate) {
        XftTextExtentsUtf8(display, font,
                           reinterpret_cast<const FcChar8 *>(candidate.c_str()),
                           static_cast<int>(candidate.size()), &extents);
        return extents.width <= maxWidth;
    };
    if (text.empty() || measure(text)) {
        return text;
    }
    const char *kDots = "\xE2\x80\xA6";  // HORIZONTAL ELLIPSIS, spelled out in UTF-8.
    std::string out = text;
    while (!out.empty()) {
        // Trim one whole UTF-8 sequence: continuation bytes, then the lead byte.
        size_t end = out.size();
        while (end > 0 && (out[end - 1] & 0xC0) == 0x80) {
            end--;
        }
        out.resize(end > 0 ? end - 1 : 0);
        std::string candidate = out + kDots;
        if (measure(candidate)) {
            return candidate;
        }
    }
    return kDots;
}

void DrawString(XftDraw *draw, const XftColor *color, XftFont *font, int x, int y,
                const std::string &text) {
    if (text.empty()) {
        return;
    }
    XftDrawStringUtf8(draw, color, font, x, y,
                      reinterpret_cast<const FcChar8 *>(text.c_str()),
                      static_cast<int>(text.size()));
}

/// Paints the closed combobox: a list-background box with a border, the routed track's
/// label, and the dropdown arrow.
void PaintCombo(WindowState *state) {
    Display *display = state->display;
    int x = kMargin;
    int y = kComboY;
    int width = static_cast<int>(kWindowWidth) - 2 * kMargin;
    int height = kComboHeight;

    XftDrawRect(state->draw, &state->listBackground, x, y, width, height);
    XSetForeground(display, state->gc, state->border.pixel);
    XDrawRectangle(display, state->window, state->gc, x, y, width - 1, height - 1);

    std::string label;
    if (state->shown.trackNo >= 0 &&
        state->shown.trackNo < static_cast<int>(state->comboLabels.size())) {
        label = state->comboLabels[static_cast<size_t>(state->shown.trackNo)];
    }
    if (!label.empty()) {
        label = Ellipsize(display, state->font, label, width - 16 - 18);
        DrawString(state->draw, &state->textStrong, state->font, x + 8, Baseline(state->font, y),
                   label);
    }
    // The arrow, pointing down: the one affordance that says this box opens.
    XPoint arrow[3] = {{static_cast<short>(x + width - 16), static_cast<short>(y + 7)},
                       {static_cast<short>(x + width - 6), static_cast<short>(y + 7)},
                       {static_cast<short>(x + width - 11), static_cast<short>(y + 13)}};
    XSetForeground(display, state->gc, state->textDim.pixel);
    XFillPolygon(display, state->window, state->gc, arrow, 3, Convex, CoordModeOrigin);
}

/// Paints the whole main window: the info rows, then the combobox.
void Paint(WindowState *state) {
    Display *display = state->display;
    XClearWindow(display, state->window);  // Background attribute: the dark palette.
    int row = 0;
    for (const Row &line : state->Rows()) {
        int top = kTop + row * kLineHeight;
        std::string text =
            Ellipsize(display, state->font, line.text,
                      static_cast<int>(kWindowWidth) - 2 * kMargin - line.indent);
        DrawString(state->draw, line.color, state->font, kMargin + line.indent,
                   Baseline(state->font, top), text);
        row++;
    }
    PaintCombo(state);
    XFlush(display);
}

/// Paints the dropped list. Caller holds popupMutex (Xlib serializes the calls anyway,
/// but the labels and scroll must be read consistently).
void PaintPopup(WindowState *state) {
    Display *display = state->display;
    XClearWindow(display, state->popup);  // Background attribute: the list background.
    int visible = state->popupHeight / kComboItemHeight;
    int count = static_cast<int>(state->comboLabels.size());
    for (int slot = 0; slot < visible; slot++) {
        int item = state->popupScroll + slot;
        if (item >= count) {
            break;
        }
        int y = slot * kComboItemHeight;
        if (item == state->shown.trackNo) {
            XftDrawRect(state->popupDraw, &state->selection, 0, y,
                        static_cast<unsigned>(state->popupWidth), kComboItemHeight);
        }
        std::string text = Ellipsize(display, state->font, state->comboLabels[static_cast<size_t>(item)],
                                     state->popupWidth - 16);
        DrawString(state->popupDraw, &state->textStrong, state->font, 8,
                   y + kComboItemHeight / 2 + (state->font->ascent - state->font->descent) / 2,
                   text);
    }
    XFlush(display);
}

/// Rebuilds the dropdown's labels when OpenUtau's track list changed. The routed
/// track's selection needs no control to follow — the paint reads shown.trackNo
/// directly, which is how host-side track changes show up here at all.
/// Caller holds popupMutex or is the event thread with the popup closed.
void SyncTracks(WindowState *state) {
    std::vector<std::string> labels;
    labels.reserve(state->shown.tracks.size());
    for (size_t i = 0; i < state->shown.tracks.size(); i++) {
        labels.push_back(TrackLabel(state->shown.tracks[i], i));
    }
    if (labels != state->comboLabels) {
        state->comboLabels = std::move(labels);
    }
}

/// Caller holds popupMutex.
void ClosePopupLocked(WindowState *state) {
    if (!state->popupOpen) {
        return;
    }
    XUngrabPointer(state->display, CurrentTime);  // No-op if the grab was already gone.
    XUnmapWindow(state->display, state->popup);
    state->popupOpen = false;
    XFlush(state->display);
}

void ClosePopup(WindowState *state) {
    std::lock_guard<std::mutex> lock(state->popupMutex);
    ClosePopupLocked(state);
}

/// Opens the dropped list under the combobox — or above it when the screen ends
/// first. A pointer grab routes every click to the popup until it closes, which is
/// how "click elsewhere to dismiss" comes for free. Event thread.
void OpenPopup(WindowState *state) {
    Display *display = state->display;
    Window root = DefaultRootWindow(display);
    std::lock_guard<std::mutex> lock(state->popupMutex);
    if (state->popupOpen || state->comboLabels.empty()) {
        return;
    }
    int count = static_cast<int>(state->comboLabels.size());
    int items = std::min(count, kPopupMaxItems);
    state->popupWidth = static_cast<int>(kWindowWidth) - 2 * kMargin;
    state->popupHeight = items * kComboItemHeight;
    int maxScroll = std::max(0, count - items);
    state->popupScroll = std::clamp(state->shown.trackNo - (items - 1) / 2, 0, maxScroll);

    // Below the combobox, flipped up when there is no room. Translated to root
    // coordinates because the popup is parented to the root — it must be free to hang
    // over the window's edge when the list is long.
    int comboBottomX = 0, comboBottomY = 0, comboTopY = 0;
    Window ignored = 0;
    XTranslateCoordinates(display, state->window, root, kMargin, kComboY + kComboHeight,
                          &comboBottomX, &comboBottomY, &ignored);
    XTranslateCoordinates(display, state->window, root, kMargin, kComboY, &comboBottomX,
                          &comboTopY, &ignored);
    int y = comboBottomY;
    if (y + state->popupHeight > DisplayHeight(display, state->screen)) {
        y = std::max(0, comboTopY - state->popupHeight);
    }
    XMoveResizeWindow(display, state->popup, comboBottomX, y, state->popupWidth,
                      state->popupHeight);
    XMapRaised(display, state->popup);
    XGrabPointer(display, state->popup, True, ButtonPressMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    state->popupOpen = true;
    PaintPopup(state);
}

/// A press on the main window: the combobox toggles the list, anything else closes it.
void HandleMainButton(WindowState *state, const XButtonEvent &event) {
    bool onCombo = event.x >= kMargin && event.x < static_cast<int>(kWindowWidth) - kMargin &&
                   event.y >= kComboY && event.y < kComboY + kComboHeight;
    bool open = false;
    {
        std::lock_guard<std::mutex> lock(state->popupMutex);
        open = state->popupOpen;
    }
    if (onCombo) {
        if (open) {
            ClosePopup(state);
        } else {
            OpenPopup(state);
        }
    } else if (open) {
        ClosePopup(state);
    }
}

/// A press while the list is dropped. The grab makes the popup the event target for
/// every click, inside or out: inside, it is a pick; outside, a dismissal.
void HandlePopupButton(WindowState *state, const XButtonEvent &event) {
    if (event.button == Button4 || event.button == Button5) {
        // Wheel scroll over the list — only ever reachable when the list was clipped.
        std::lock_guard<std::mutex> lock(state->popupMutex);
        if (!state->popupOpen) {
            return;
        }
        int visible = state->popupHeight / kComboItemHeight;
        int maxScroll =
            std::max(0, static_cast<int>(state->comboLabels.size()) - visible);
        int delta = event.button == Button5 ? 1 : -1;
        state->popupScroll = std::clamp(state->popupScroll + delta, 0, maxScroll);
        PaintPopup(state);
        return;
    }

    int picked = -1;
    {
        std::lock_guard<std::mutex> lock(state->popupMutex);
        if (!state->popupOpen) {
            return;
        }
        bool inside = event.x >= 0 && event.y >= 0 && event.x < state->popupWidth &&
                      event.y < state->popupHeight;
        int item = state->popupScroll + event.y / kComboItemHeight;
        if (inside && item >= 0 && item < static_cast<int>(state->comboLabels.size())) {
            picked = item;
            ClosePopupLocked(state);
        } else if (!inside) {
            ClosePopupLocked(state);
        }
    }
    if (picked >= 0) {
        // The session's routing change is atomic; the host nudge is the one call that
        // leaves the event thread — see the file header for why that is accepted.
        state->session->RequestTrackNo(picked);
        if (state->onTrackPicked) {
            state->onTrackPicked();
        }
    }
}

/// The shared hide path: close the list, detach from whatever we were embedded in,
/// unmap. Runs on the main thread (gui hide) and on the event thread (the floating
/// window's close box); both end in the same place, and Xlib serializes the calls.
void HideWindow(WindowState *state) {
    ClosePopup(state);
    Display *display = state->display;
    if (state->parent != 0) {
        XReparentWindow(display, state->window, DefaultRootWindow(display), 0, 0);
        state->parent = 0;
        state->floating = true;
    }
    XUnmapWindow(display, state->window);
    XFlush(display);
}

/// One select()-bounded round of the event loop: drain the queue, and when the queue
/// stayed empty for a tick, repaint at 4 Hz — the timer the Win32 backend gets from
/// SetTimer and Cocoa from NSTimer, built here out of the connection's file descriptor.
void EventLoop(WindowState *state) {
    Display *display = state->display;
    int fd = ConnectionNumber(display);
    while (state->running) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        timeval timeout{kTickMs / 1000, static_cast<suseconds_t>((kTickMs % 1000) * 1000)};
        int ready = select(fd + 1, &readable, nullptr, nullptr, &timeout);
        if (!state->running) {
            return;
        }
        while (state->running && XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            switch (event.type) {
                case Expose:
                    if (event.xexpose.window == state->window) {
                        Paint(state);
                    } else if (event.xexpose.window == state->popup) {
                        std::lock_guard<std::mutex> lock(state->popupMutex);
                        if (state->popupOpen) {
                            PaintPopup(state);
                        }
                    }
                    break;
                case ButtonPress:
                    if (event.xbutton.window == state->popup) {
                        HandlePopupButton(state, event.xbutton);
                    } else if (event.xbutton.window == state->window) {
                        HandleMainButton(state, event.xbutton);
                    }
                    break;
                case ClientMessage:
                    if (event.xclient.window == state->window &&
                        static_cast<Atom>(event.xclient.data.l[0]) == state->deleteAtom) {
                        // A floating window closed by its user hides rather than
                        // destroys: the host owns the gui's lifetime, and it was not
                        // asked for destroy().
                        HideWindow(state);
                    }
                    break;
                default:
                    break;
            }
        }
        if (ready == 0) {
            // Unconditional: copy, sync and repaint at 4 Hz, self-healing whatever the
            // X server discarded — the same reasoning as the Win32 backend's timer.
            state->shown = state->session->UiCopy();
            {
                std::lock_guard<std::mutex> lock(state->popupMutex);
                // While the list is dropped open, leave the labels alone: the user is
                // browsing, and a rebuild under the cursor would move the items.
                if (!state->popupOpen) {
                    SyncTracks(state);
                }
            }
            Paint(state);
            {
                std::lock_guard<std::mutex> lock(state->popupMutex);
                if (state->popupOpen) {
                    PaintPopup(state);
                }
            }
        }
    }
}

/// Builds everything X-side from scratch. Used at creation; unlike Windows, an X window
/// does not die behind our back when a host reloads its UI, so there is no rebuild path.
bool CreateNative(WindowState *state) {
    Display *display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return false;
    }
    state->display = display;
    state->screen = DefaultScreen(display);
    state->colormap = DefaultColormap(display, state->screen);
    Visual *visual = DefaultVisual(display, state->screen);
    Window root = DefaultRootWindow(display);

    const auto alloc = [&](const Rgb &rgb, XftColor *color) {
        XRenderColor render{static_cast<unsigned short>((rgb.r << 8) | rgb.r),
                            static_cast<unsigned short>((rgb.g << 8) | rgb.g),
                            static_cast<unsigned short>((rgb.b << 8) | rgb.b), 0xFFFF};
        return XftColorAllocValue(display, visual, state->colormap, &render, color) != 0;
    };
    if (!alloc(kBackground, &state->background) || !alloc(kListBackground, &state->listBackground) ||
        !alloc(kSelection, &state->selection) || !alloc(kBorder, &state->border) ||
        !alloc(kTextStrong, &state->textStrong) || !alloc(kTextDim, &state->textDim) ||
        !alloc(kAccent, &state->accent)) {
        XCloseDisplay(display);
        state->display = nullptr;
        return false;
    }

    // The font: 10 point of the default sans. Track names are arbitrary UTF-8, which is
    // the whole reason for Xft; without fontconfig there is no window.
    state->font = XftFontOpenName(display, state->screen, "Sans-10");
    if (state->font == nullptr) {
        state->font = XftFontOpenName(display, state->screen, "sans-serif-10");
    }
    if (state->font == nullptr) {
        XCloseDisplay(display);
        state->display = nullptr;
        return false;
    }

    state->window =
        XCreateSimpleWindow(display, root, 0, 0, kWindowWidth, kWindowHeight, 0, 0,
                            state->background.pixel);
    if (state->window == 0) {
        XftFontClose(display, state->font);
        XCloseDisplay(display);
        state->display = nullptr;
        return false;
    }
    XSelectInput(display, state->window, ExposureMask | ButtonPressMask | StructureNotifyMask);

    // A fixed size, told to the window manager in every way it might listen.
    XSizeHints *sizeHints = XAllocSizeHints();
    if (sizeHints != nullptr) {
        sizeHints->flags = PBaseSize | PMinSize | PMaxSize;
        sizeHints->base_width = sizeHints->min_width = sizeHints->max_width = kWindowWidth;
        sizeHints->base_height = sizeHints->min_height = sizeHints->max_height = kWindowHeight;
        XSetWMNormalHints(display, state->window, sizeHints);
        XFree(sizeHints);
    }
    XClassHint *classHints = XAllocClassHint();
    if (classHints != nullptr) {
        classHints->res_name = const_cast<char *>("openutau-bridge");
        classHints->res_class = const_cast<char *>("OpenUtauBridge");
        XSetClassHint(display, state->window, classHints);
        XFree(classHints);
    }
    state->deleteAtom = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, state->window, &state->deleteAtom, 1);
    // Both title channels: XStoreName for the legacy clients, _NET_WM_NAME (UTF-8) for
    // every current window manager — the host's suggested title can be any project name.
    XStoreName(display, state->window, state->title.c_str());
    XChangeProperty(display, state->window, XInternAtom(display, "_NET_WM_NAME", False),
                    XInternAtom(display, "UTF8_STRING", False), 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(state->title.c_str()),
                    static_cast<int>(state->title.size()));

    // The dropped list: override-redirect (no window manager between us and the
    // screen), parented to the root so it can overhang the window, shown only on open.
    XSetWindowAttributes popupAttributes{};
    popupAttributes.override_redirect = True;
    popupAttributes.background_pixel = state->listBackground.pixel;
    popupAttributes.save_under = True;
    popupAttributes.event_mask = ExposureMask | ButtonPressMask;
    state->popup = XCreateWindow(display, root, 0, 0, 1, 1, 0, CopyFromParent, InputOutput,
                                 CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel | CWEventMask | CWSaveUnder,
                                 &popupAttributes);

    state->gc = XCreateGC(display, state->window, 0, nullptr);
    state->draw = XftDrawCreate(display, state->window, visual, state->colormap);
    // Only draw into the popup if it exists: an XftDraw on a zero drawable would raise
    // a BadDrawable X error, whose default handler ends the process.
    if (state->popup != 0) {
        state->popupDraw = XftDrawCreate(display, state->popup, visual, state->colormap);
    }
    if (state->gc == nullptr || state->draw == nullptr || state->popup == 0 ||
        state->popupDraw == nullptr) {
        // Nothing has been mapped yet: tear the X-side down and report no gui. The
        // draws go before their windows — see the destructor for the ordering rule.
        if (state->popupDraw != nullptr) {
            XftDrawDestroy(state->popupDraw);
        }
        if (state->draw != nullptr) {
            XftDrawDestroy(state->draw);
        }
        if (state->popup != 0) {
            XDestroyWindow(display, state->popup);
        }
        XDestroyWindow(display, state->window);
        if (state->gc != nullptr) {
            XFreeGC(display, state->gc);
        }
        if (state->font != nullptr) {
            XftFontClose(display, state->font);
        }
        XCloseDisplay(display);
        state->display = nullptr;
        state->window = 0;
        state->popup = 0;
        return false;
    }

    // The first frame, before anything can map the window, and then the event thread
    // that keeps it alive.
    SyncTracks(state);
    Paint(state);
    state->running = true;
    state->eventThread = std::thread(EventLoop, state);
    return true;
}

}  // namespace

}  // namespace x11gui

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
               std::strcmp(api, CLAP_WINDOW_API_X11) == 0;
    }
    return api != nullptr && std::strcmp(api, CLAP_WINDOW_API_X11) == 0;
}

bool IsX11Window(const clap_window_t *window) {
    return window != nullptr && window->api != nullptr &&
           std::strcmp(window->api, CLAP_WINDOW_API_X11) == 0;
}

const clap_plugin_gui_t &GuiTable() {
    static const clap_plugin_gui_t kGui = {
        // is_api_supported
        +[](const clap_plugin_t *, const char *api, bool isFloating) {
            return Supported(api, isFloating);
        },
        // get_preferred_api
        +[](const clap_plugin_t *, const char **api, bool *isFloating) {
            *api = CLAP_WINDOW_API_X11;
            // Floating, so the window behaves the same in every host that shows one.
            *isFloating = true;
            return true;
        },
        // create — the window already exists per instance; a host that embeds re-parents
        // it in set_parent below.
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
        // set_scale — physical pixels are used as-is, so a scale factor is not applied
        // and is reported as ignored rather than silently accepted.
        +[](const clap_plugin_t *, double) { return false; },
        // get_size
        +[](const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
            if (Instance(plugin) == nullptr) {
                return false;
            }
            *width = x11gui::kWindowWidth;
            *height = x11gui::kWindowHeight;
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
            *width = x11gui::kWindowWidth;
            *height = x11gui::kWindowHeight;
            return true;
        },
        // set_size — accepted so a host restoring a session is not told no, and then
        // overridden: the window draws itself at its fixed size regardless.
        +[](const clap_plugin_t *, uint32_t, uint32_t) { return true; },
        // set_parent
        +[](const clap_plugin_t *plugin, const clap_window_t *window) {
            InfoWindow *info = Instance(plugin);
            return info != nullptr && IsX11Window(window) &&
                   (info->EmbedInto(reinterpret_cast<void *>(static_cast<uintptr_t>(window->x11))),
                    true);
        },
        // set_transient
        +[](const clap_plugin_t *plugin, const clap_window_t *window) {
            InfoWindow *info = Instance(plugin);
            return info != nullptr && IsX11Window(window) &&
                   (info->OwnTo(reinterpret_cast<void *>(static_cast<uintptr_t>(window->x11))),
                    true);
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
    using x11gui::WindowState;
    // Our event thread touches the same display connection the main thread uses; Xlib
    // must be told once, before the first display opens, that this is going to happen.
    static std::once_flag threadsInit;
    std::call_once(threadsInit, [] { XInitThreads(); });

    auto *state = new WindowState();
    state->session = session;
    state->onTrackPicked = std::move(onTrackPicked);
    state->shown = session->UiCopy();
    if (!x11gui::CreateNative(state)) {
        // Null means no gui — the plugin stays a degraded but working plugin, exactly
        // like the stub path.
        delete state;
        return nullptr;
    }
    auto *window = new InfoWindow();
    window->impl_ = state;
    return window;
}

InfoWindow::~InfoWindow() {
    auto *state = static_cast<x11gui::WindowState *>(impl_);
    if (state == nullptr) {
        return;
    }
    if (state->eventThread.joinable()) {
        state->running = false;
        state->eventThread.join();  // The select timeout wakes it within one tick.
    }
    Display *display = state->display;
    if (display != nullptr) {
        x11gui::ClosePopup(state);
        // Free the draws before their windows: an XftDraw wraps a Render picture tied
        // to the drawable, and freeing a picture the server already released with its
        // window raises a BadPicture error, whose default handler ends the process.
        if (state->popupDraw != nullptr) {
            XftDrawDestroy(state->popupDraw);
        }
        if (state->draw != nullptr) {
            XftDrawDestroy(state->draw);
        }
        if (state->popup != 0) {
            XDestroyWindow(display, state->popup);
        }
        if (state->window != 0) {
            XDestroyWindow(display, state->window);
        }
        if (state->gc != nullptr) {
            XFreeGC(display, state->gc);
        }
        if (state->font != nullptr) {
            XftFontClose(display, state->font);
        }
        XCloseDisplay(display);  // The XftColors go with the connection.
    }
    delete state;
}

const clap_plugin_gui_t *InfoWindow::Extension() const { return &GuiTable(); }

bool InfoWindow::EnsureAlive() {
    auto *state = static_cast<x11gui::WindowState *>(impl_);
    // An X window is not the host's to destroy — the only teardown is ours — so alive
    // here is simply whether the window was ever built.
    return state != nullptr && state->alive();
}

void InfoWindow::Show() {
    auto *state = static_cast<x11gui::WindowState *>(impl_);
    if (state == nullptr || !state->alive()) {
        return;
    }
    // No centering pass: the window manager places the floating window, and embedded
    // windows are exactly where set_parent put them. The 4 Hz tick repaints whatever
    // changed since the last show.
    XMapRaised(state->display, state->window);
    XFlush(state->display);
}

void InfoWindow::Hide() {
    auto *state = static_cast<x11gui::WindowState *>(impl_);
    if (state == nullptr || !state->alive()) {
        return;
    }
    // Detach before hiding, as on Windows: while the editor is closed the window must
    // not remain a child of host UI that a reload may destroy.
    x11gui::HideWindow(state);
}

void InfoWindow::EmbedInto(void *parent) {
    auto *state = static_cast<x11gui::WindowState *>(impl_);
    if (state == nullptr || !state->alive() || parent == nullptr) {
        return;
    }
    auto host = static_cast<Window>(reinterpret_cast<uintptr_t>(parent));
    state->parent = host;
    state->owner = 0;
    state->floating = false;
    // A child of the host's window, at its origin, at our fixed size — the embed must
    // place it itself, exactly as on Windows.
    XReparentWindow(state->display, state->window, host, 0, 0);
    XResizeWindow(state->display, state->window, x11gui::kWindowWidth, x11gui::kWindowHeight);
    XMapWindow(state->display, state->window);
    XFlush(state->display);
}

void InfoWindow::OwnTo(void *owner) {
    auto *state = static_cast<x11gui::WindowState *>(impl_);
    if (state == nullptr || !state->alive() || owner == nullptr) {
        return;
    }
    auto host = static_cast<Window>(reinterpret_cast<uintptr_t>(owner));
    state->owner = host;
    // The transient hint is X11's owner equivalent: the window manager keeps us above
    // the host window and grouped with it.
    XSetTransientForHint(state->display, state->window, host);
    XFlush(state->display);
}

void InfoWindow::Retitle(const char *title) {
    auto *state = static_cast<x11gui::WindowState *>(impl_);
    if (state == nullptr || !state->alive() || title == nullptr) {
        return;
    }
    state->title = title;
    XStoreName(state->display, state->window, title);
    XChangeProperty(state->display, state->window,
                    XInternAtom(state->display, "_NET_WM_NAME", False),
                    XInternAtom(state->display, "UTF8_STRING", False), 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(title),
                    static_cast<int>(state->title.size()));
    XFlush(state->display);
}

bool InfoWindow::SetContentScale(float) {
    return false;  // Physical pixels throughout; see the gui table's set_scale.
}

}  // namespace bridge

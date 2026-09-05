/*
 * The info window's Cocoa backend: AppKit controls — one text field per info row and an
 * NSPopUpButton for the track picker — shown floating in a plain titled window, or added
 * as a subview of the host's view for embedded mode. No dependency beyond the system
 * frameworks: like the Win32 backend, this is a glance plus a dropdown, not a UI
 * framework's worth of surface.
 *
 * Same contract as gui_win32.cpp: main thread only (the CLAP gui extension's own
 * requirement), UiCopy() as the sole cross-thread touch, instances registered by
 * plugin.cpp and looked up per gui callback. Compiled as Objective-C++ with ARC.
 */

#import <Cocoa/Cocoa.h>

#include "gui.h"
#include "session.h"

#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// The dropdown item for a track: "N: name - singer - engine", with the informational
// fields simply left out when OpenUtau reports none.
static std::string TrackLabel(const bridge::TrackInfo &track, size_t index) {
    std::string label = std::to_string(index + 1) + ": " + track.name;
    if (!track.singer.empty()) {
        label += "  \xE2\x80\x94  " + track.singer;  // em dash
    }
    if (!track.engine.empty()) {
        label += "  \xC2\xB7  " + track.engine;  // middle dot
    }
    return label;
}

/// The panel: five labels for the info rows and the track picker, plus the refresh
/// timer. Declared at global scope — Objective-C classes cannot live in namespaces —
/// under a name specific enough to stay out of the host's way.
@interface OpenUtauBridgePanel : NSView {
    @public
    bridge::Session *_session;
    std::function<void()> _onTrackPicked;
    NSTextField *_connection;
    NSTextField *_project;
    NSTextField *_tempo;
    NSTextField *_transport;
    NSTextField *_singer;
    NSPopUpButton *_tracks;
    NSTimer *_timer;
}

- (instancetype)initWithSession:(bridge::Session *)session
                 onTrackPicked:(std::function<void()>)onTrackPicked;
- (void)refresh;   // one UiCopy() sync: rebuild the picker, redraw the rows
- (void)startTimer;
- (void)stopTimer;

@end

@implementation OpenUtauBridgePanel

- (instancetype)initWithSession:(bridge::Session *)session
                 onTrackPicked:(std::function<void()>)onTrackPicked {
    self = [super initWithFrame:NSMakeRect(0, 0, 320, 160)];
    if (self == nil) {
        return self;
    }
    _session = session;
    _onTrackPicked = std::move(onTrackPicked);

    NSFont *font = [NSFont systemFontOfSize:13];
    auto makeLabel = ^(NSRect frame) {
        NSTextField *label = [NSTextField labelWithString:@""];
        label.frame = frame;
        label.font = font;
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:label];
        return label;
    };
    // Rows top to bottom: connection, project, tempo, transport, singer/engine — the
    // same lines the Win32 backend paints, here in real labels.
    CGFloat rowHeight = 22, margin = 12;
    _connection = makeLabel(NSMakeRect(margin, 160 - margin - rowHeight * 1, 296, rowHeight));
    _project = makeLabel(NSMakeRect(margin, 160 - margin - rowHeight * 2, 296, rowHeight));
    _tempo = makeLabel(NSMakeRect(margin, 160 - margin - rowHeight * 3, 296, rowHeight));
    _transport = makeLabel(NSMakeRect(margin, 160 - margin - rowHeight * 4, 296, rowHeight));
    _singer = makeLabel(NSMakeRect(margin, 160 - margin - rowHeight * 5, 296, rowHeight));

    _tracks = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(margin, margin, 296, 26)
        pullsDown:NO];
    _tracks.font = font;
    // The action fires on the user's pick only; selectItemAtIndex from refresh() does
    // not raise it, so host-side track changes cannot echo back into a request.
    [_tracks setTarget:self];
    [_tracks setAction:@selector(trackPicked:)];
    [self addSubview:_tracks];

    [self refresh];
    return self;
}

- (void)trackPicked:(id)sender {
    (void)sender;
    NSInteger picked = [_tracks indexOfSelectedItem];
    if (picked >= 0) {
        _session->RequestTrackNo(static_cast<int>(picked));
        if (_onTrackPicked) {
            _onTrackPicked();
        }
    }
}

- (void)set:(NSTextField *)label string:(NSString *)text secondary:(BOOL)secondary {
    label.stringValue = text;
    label.textColor = secondary ? NSColor.secondaryLabelColor : NSColor.labelColor;
}

- (void)refresh {
    bridge::UiState current = _session->UiCopy();

    // The picker: rebuild only when OpenUtau's track list changed, then follow the
    // routed track's selection.
    std::vector<std::string> labels;
    labels.reserve(current.tracks.size());
    for (size_t i = 0; i < current.tracks.size(); i++) {
        labels.push_back(TrackLabel(current.tracks[i], i));
    }
    // Rebuild only when the list actually changed; keys are cheap string fingerprints.
    if ([self popupItemsKey] != [self labelsKey:labels]) {
        [_tracks removeAllItems];
        for (const std::string &label : labels) {
            [_tracks addItemWithTitle:[NSString stringWithUTF8String:label.c_str()]];
        }
    }
    NSInteger count = static_cast<NSInteger>(current.tracks.size());
    if (count > 0 && current.trackNo >= 0 &&
        current.trackNo < static_cast<int>(count)) {
        if ([_tracks indexOfSelectedItem] != current.trackNo) {
            [_tracks selectItemAtIndex:current.trackNo];
        }
    } else {
        [_tracks selectItem:nil];
    }

    // The rows.
    _connection.stringValue = current.connected
        ? [NSString stringWithFormat:@"Connected on port %d", current.port]
        : @"Not connected";
    _connection.textColor =
        current.connected ? NSColor.controlAccentColor : NSColor.secondaryLabelColor;

    if (current.projectSaved && !current.projectName.empty()) {
        [self set:_project
            string:[NSString stringWithFormat:@"Project: %s", current.projectName.c_str()]
          secondary:NO];
    } else {
        [self set:_project string:@"Project: (unsaved)" secondary:NO];
    }

    _tempo.stringValue = current.hasTempo
        ? [NSString stringWithFormat:@"Tempo: %d BPM",
                                     static_cast<int>(current.tempo + 0.5)]
        : @"Tempo: (unknown)";
    _tempo.textColor = NSColor.labelColor;

    _transport.stringValue =
        current.playing ? @"Transport: playing" : @"Transport: stopped";
    _transport.textColor =
        current.playing ? NSColor.controlAccentColor : NSColor.labelColor;

    if (current.trackNo >= 0 &&
        current.trackNo < static_cast<int>(current.tracks.size())) {
        const bridge::TrackInfo &track =
            current.tracks[static_cast<size_t>(current.trackNo)];
        NSString *who = track.singer.empty()
            ? @"(none)"
            : [NSString stringWithUTF8String:track.singer.c_str()];
        NSString *engine = track.engine.empty()
            ? @"(none)"
            : [NSString stringWithUTF8String:track.engine.c_str()];
        _singer.stringValue = [NSString
            stringWithFormat:@"Track %d \xE2\x80\x94 singer: %@ \xC2\xB7 engine: %@",
                             current.trackNo + 1, who, engine];
        _singer.textColor = NSColor.labelColor;
    } else {
        _singer.stringValue = @"No tracks reported yet.";
        _singer.textColor = NSColor.secondaryLabelColor;
    }
}

/// A comparable fingerprint of what the popup lists, so a refresh can tell "same list"
/// from "rebuild" without poking the control.
- (std::string)popupItemsKey {
    std::string key;
    for (NSString *title in _tracks.itemTitles) {
        key += title.UTF8String;
        key += '\n';
    }
    return key;
}

- (std::string)labelsKey:(const std::vector<std::string> &)labels {
    std::string key;
    for (const std::string &label : labels) {
        key += label;
        key += '\n';
    }
    return key;
}

- (void)timerTick:(NSTimer *)timer {
    (void)timer;
    [self refresh];
}

- (void)startTimer {
    if (_timer == nil) {
        _timer = [NSTimer timerWithTimeInterval:0.25
                                         target:self
                                       selector:@selector(timerTick:)
                                       userInfo:nil
                                        repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
    }
}

- (void)stopTimer {
    [_timer invalidate];
    _timer = nil;
}

- (void)dealloc {
    [self stopTimer];
}

@end

namespace bridge {

namespace cocoagui {

constexpr CGFloat kWindowWidth = 320;
constexpr CGFloat kWindowHeight = 160;

/// The panel plus its floating wrapper. In embedded mode the panel lives in the host's
/// view and the wrapper is unused; `floatingMode` says which world we are in.
struct WindowState {
    Session *session = nullptr;
    std::function<void()> onTrackPicked;
    OpenUtauBridgePanel *panel = nil;
    NSWindow *floating = nil;
    bool floatingMode = true;
};

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
               std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
    }
    return api != nullptr && std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
}

bool IsCocoaWindow(const clap_window_t *window) {
    return window != nullptr && window->api != nullptr &&
           std::strcmp(window->api, CLAP_WINDOW_API_COCOA) == 0;
}

const clap_plugin_gui_t &GuiTable() {
    static const clap_plugin_gui_t kGui = {
        // is_api_supported
        +[](const clap_plugin_t *, const char *api, bool isFloating) {
            return Supported(api, isFloating);
        },
        // get_preferred_api
        +[](const clap_plugin_t *, const char **api, bool *isFloating) {
            *api = CLAP_WINDOW_API_COCOA;
            // Floating, so the window behaves the same in every host that shows one.
            *isFloating = true;
            return true;
        },
        // create — the panel already exists per instance; a host that embeds re-parents
        // it in set_parent below.
        +[](const clap_plugin_t *plugin, const char *, bool) {
            return Instance(plugin) != nullptr;
        },
        // destroy — only hides; the panel itself dies when the plugin is destroyed.
        +[](const clap_plugin_t *plugin) {
            InfoWindow *info = Instance(plugin);
            if (info != nullptr) {
                info->Hide();
            }
        },
        // set_scale — points, not physical pixels; a scale factor is not applied and is
        // reported as ignored rather than silently accepted.
        +[](const clap_plugin_t *, double) { return false; },
        // get_size
        +[](const clap_plugin_t *plugin, uint32_t *width, uint32_t *height) {
            if (Instance(plugin) == nullptr) {
                return false;
            }
            *width = static_cast<uint32_t>(kWindowWidth);
            *height = static_cast<uint32_t>(kWindowHeight);
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
            *width = static_cast<uint32_t>(kWindowWidth);
            *height = static_cast<uint32_t>(kWindowHeight);
            return true;
        },
        // set_size — accepted so a host restoring a session is not told no, and then
        // overridden: the panel draws itself at its fixed size regardless.
        +[](const clap_plugin_t *, uint32_t, uint32_t) { return true; },
        // set_parent
        +[](const clap_plugin_t *plugin, const clap_window_t *window) {
            InfoWindow *info = Instance(plugin);
            return info != nullptr && IsCocoaWindow(window) &&
                   (info->EmbedInto(window->cocoa), true);
        },
        // set_transient
        +[](const clap_plugin_t *plugin, const clap_window_t *window) {
            InfoWindow *info = Instance(plugin);
            return info != nullptr && IsCocoaWindow(window) &&
                   (info->OwnTo(window->cocoa), true);
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

}  // namespace cocoagui

void GuiRegister(const clap_plugin_t *plugin, InfoWindow *window) {
    std::lock_guard<std::mutex> lock(cocoagui::RegistryMutex());
    cocoagui::Registry()[plugin] = window;
}

void GuiUnregister(const clap_plugin_t *plugin) {
    std::lock_guard<std::mutex> lock(cocoagui::RegistryMutex());
    cocoagui::Registry().erase(plugin);
}

InfoWindow *CreateInfoWindow(Session *session, std::function<void()> onTrackPicked) {
    using cocoagui::WindowState;
    // Hosts that show plugin editors already run AppKit, but make sure the shared
    // application exists so control creation can never return nil for lack of one.
    [NSApplication sharedApplication];

    auto *state = new WindowState();
    state->session = session;
    state->onTrackPicked = std::move(onTrackPicked);
    state->panel = [[OpenUtauBridgePanel alloc] initWithSession:session
                                                  onTrackPicked:state->onTrackPicked];
    if (state->panel == nil) {
        delete state;
        return nullptr;
    }

    state->floating = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, cocoagui::kWindowWidth,
                                       cocoagui::kWindowHeight)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
        backing:NSBackingStoreBuffered
        defer:NO];
    state->floating.title = @"OpenUtau Bridge";
    state->floating.releasedWhenClosed = NO;
    state->floating.contentView = state->panel;

    auto *window = new InfoWindow();
    window->impl_ = state;
    return window;
}

InfoWindow::~InfoWindow() {
    auto *state = static_cast<cocoagui::WindowState *>(impl_);
    if (state == nullptr) {
        return;
    }
    [state->panel stopTimer];
    [state->panel removeFromSuperview];  // Detaches from host or wrapper, then ARC frees.
    [state->floating orderOut:nil];
    state->floating = nil;
    delete state;
}

const clap_plugin_gui_t *InfoWindow::Extension() const { return &cocoagui::GuiTable(); }

void InfoWindow::Show() {
    auto *state = static_cast<cocoagui::WindowState *>(impl_);
    if (state->floatingMode) {
        [state->floating center];  // Over whatever the host last had frontmost: the
                                   // position is a convenience, not a contract.
        [state->floating orderFrontRegardless];
    }
    [state->panel refresh];
    [state->panel startTimer];
}

void InfoWindow::Hide() {
    auto *state = static_cast<cocoagui::WindowState *>(impl_);
    if (state->floatingMode) {
        [state->floating orderOut:nil];
    }
    [state->panel stopTimer];
}

void InfoWindow::EmbedInto(void *parent) {
    auto *state = static_cast<cocoagui::WindowState *>(impl_);
    // The host hands a raw NSView*; under ARC the void* round-trip needs a bridged cast
    // (ownership stays with the host's view hierarchy, which is what __bridge says).
    NSView *host = (__bridge NSView *)parent;
    [state->panel removeFromSuperview];
    [host addSubview:state->panel];
    state->panel.frame = host.bounds;
    state->panel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    state->floatingMode = false;  // The panel now lives in the host's hierarchy.
}

void InfoWindow::OwnTo(void *owner) {
    // NSWindow has no transient-owner concept to set; the host's suggestion is
    // acknowledged without effect.
    (void)owner;
}

void InfoWindow::Retitle(const char *title) {
    auto *state = static_cast<cocoagui::WindowState *>(impl_);
    if (state->floatingMode) {
        state->floating.title = [NSString stringWithUTF8String:title];
    }
}

bool InfoWindow::SetContentScale(float) {
    return false;  // Points throughout; see the gui table's set_scale.
}

}  // namespace bridge

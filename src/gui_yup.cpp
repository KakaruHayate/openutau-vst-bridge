/*
 * The plugin window's YUP backend (https://github.com/kunitoki/yup, ISC): one component
 * tree on every platform, replacing the hand-rolled Win32 window of the 1.1 builds.
 *
 * The window is a glance, not a mixer: a track picker (a combo box fed from updateTracks,
 * whose change is routed through the session so the host sees a real parameter movement),
 * plus read-only rows for the connection, the project, the routed track's singer and
 * render engine, the tempo and the transport. A 4 Hz timer repaints from UiCopy(), which
 * is built for exactly this cross-thread read; the same change-detection as before means
 * an unchanged state costs nothing.
 *
 * The extension's callbacks receive the clap_plugin_t, not the window, so instances live
 * in a registry keyed by the plugin pointer, filled by plugin.cpp around init/destroy —
 * the same shape the Win32 backend used.
 */

#include "gui.h"

#include "session.h"

#include <yup_gui/yup_gui.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace bridge {

namespace {

// Same footprint the Win32 window had; a glance, not an editor panel. Height covers the
// picker row plus five info rows (12 + 36 + 5 * 28) and the bottom margin.
constexpr float kWindowWidth = 320.0f;
constexpr float kWindowHeight = 194.0f;

const char *WindowApi() {
#if YUP_WINDOWS
    return CLAP_WINDOW_API_WIN32;
#elif YUP_MAC
    return CLAP_WINDOW_API_COCOA;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

/// The native parent handle a clap_window_t carries on this platform, or null.
void *ParentHandle(const clap_window_t *window) {
    if (window == nullptr || window->api == nullptr ||
        std::strcmp(window->api, WindowApi()) != 0) {
        return nullptr;
    }
#if YUP_WINDOWS
    return window->win32;
#elif YUP_MAC
    return window->cocoa;
#else
    return window->x11;
#endif
}

yup::String Utf8(const std::string &text) {
    return text.empty() ? yup::String() : yup::String::fromUTF8(text.data(),
                                                                static_cast<int>(text.size()));
}

/// "1: Lead — Test Singer · DIFFSINGER" — what both the picker and the info row show.
std::string TrackLabel(int oneBased, const TrackInfo &track) {
    std::string label = std::to_string(oneBased) + ": " + track.name;
    if (!track.singer.empty()) {
        label += " \xE2\x80\x94 " + track.singer;  // U+2014 em dash.
    }
    if (!track.engine.empty()) {
        label += " \xC2\xB7 " + track.engine;  // U+00B7 middle dot.
    }
    return label;
}

/// Everything drawn, and the last snapshot it was drawn from.
struct EditorState {
    Session *session = nullptr;
    std::function<void()> onTrackPicked;  ///< Nudges the host after a picker change.

    std::unique_ptr<yup::ComboBox> trackBox;
    std::unique_ptr<yup::Label> connection;
    std::unique_ptr<yup::Label> project;
    std::unique_ptr<yup::Label> singer;
    std::unique_ptr<yup::Label> tempo;
    std::unique_ptr<yup::Label> transport;

    UiState shown;
    std::string trackSignature;

    static constexpr float kMargin = 12.0f;
    static constexpr float kRowHeight = 22.0f;
    static constexpr float kRowGap = 6.0f;

    explicit EditorState(Session &s) : session(&s) {
        trackBox = std::make_unique<yup::ComboBox>("trackPicker");
        trackBox->onSelectedItemChanged = [this] {
            const int id = trackBox->getSelectedId();
            if (id <= 0 || id - 1 == shown.trackNo) {
                return;  // A refresh echo, not a user pick.
            }
            shown.trackNo = id - 1;
            session->RequestTrackNo(id - 1);
            if (onTrackPicked) {
                onTrackPicked();
            }
        };

        connection = std::make_unique<yup::Label>("connection");
        project = std::make_unique<yup::Label>("project");
        singer = std::make_unique<yup::Label>("singer");
        tempo = std::make_unique<yup::Label>("tempo");
        transport = std::make_unique<yup::Label>("transport");

        shown = session->UiCopy();
        Refresh(true);
    }

    /// Rebuilds the picker when the track set's content changed, then rewrites the labels.
    /// `force` is for the first pass, where every widget starts empty.
    void Refresh(bool force = false) {
        UiState current = session->UiCopy();

        std::string signature;
        for (const TrackInfo &track : current.tracks) {
            signature += track.name + '\x01' + track.singer + '\x01' + track.engine + '\n';
        }
        if (force || signature != trackSignature) {
            trackSignature = signature;
            trackBox->clear();
            if (current.tracks.empty()) {
                // Before the first updateTracks lands the picker still has to offer
                // something: the numbered tracks a project will have once connected.
                for (int i = 1; i <= 64; i++) {
                    trackBox->addItem(Utf8(std::to_string(i)), i);
                }
            } else {
                int oneBased = 1;
                for (const TrackInfo &track : current.tracks) {
                    trackBox->addItem(Utf8(TrackLabel(oneBased, track)), oneBased);
                    oneBased++;
                }
            }
            force = true;  // The selection below has to be re-applied after a rebuild.
        }

        if (force || trackBox->getSelectedId() != current.trackNo + 1) {
            trackBox->setSelectedId(current.trackNo + 1, yup::dontSendNotification);
        }

        connection->setText(current.connected
                                ? Utf8("\xE2\x97\x8F Connected on port " + std::to_string(current.port))
                                : Utf8("\xE2\x97\x8B Not connected"));
        project->setText(current.projectSaved && !current.projectName.empty()
                             ? Utf8("Project: " + current.projectName)
                             : Utf8("Project: (unsaved)"));
        singer->setText(Utf8(SingerText(current)));
        tempo->setText(current.hasTempo
                           ? Utf8("Tempo: " +
                                  std::to_string(static_cast<int>(current.tempo + 0.5)) + " BPM")
                           : Utf8("Tempo: (unknown)"));
        transport->setText(current.playing ? Utf8("Transport: playing")
                                           : Utf8("Transport: stopped"));

        shown = current;
    }

    std::string SingerText(const UiState &state) const {
        const int index = state.trackNo;
        if (state.tracks.empty() || index < 0 || index >= static_cast<int>(state.tracks.size())) {
            return "Track: (no tracks reported yet)";
        }
        const TrackInfo &track = state.tracks[static_cast<size_t>(index)];
        if (track.singer.empty() && track.engine.empty()) {
            return "Track " + std::to_string(index + 1) + ": (no singer yet)";
        }
        if (track.engine.empty()) {
            return "Track " + std::to_string(index + 1) + ": " + track.singer;
        }
        if (track.singer.empty()) {
            return "Track " + std::to_string(index + 1) + ": (" + track.engine + ")";
        }
        return "Track " + std::to_string(index + 1) + ": " + track.singer + " \xC2\xB7 " +
               track.engine;
    }

    void Layout() {
        float y = kMargin;
        trackBox->setBounds(kMargin, y, kWindowWidth - 2 * kMargin, 28.0f);
        y += 36.0f;
        for (yup::Label *label : {connection.get(), project.get(), singer.get(), tempo.get(),
                                  transport.get()}) {
            label->setBounds(kMargin, y, kWindowWidth - 2 * kMargin, kRowHeight);
            y += kRowHeight + kRowGap;
        }
    }
};

// --------------------------------------------------------------------- the registry

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
        return api == nullptr || api[0] == '\0' || std::strcmp(api, WindowApi()) == 0;
    }
    return api != nullptr && std::strcmp(api, WindowApi()) == 0;
}

bool IsHostWindow(const clap_window_t *window) {
    return window != nullptr && window->api != nullptr &&
           std::strcmp(window->api, WindowApi()) == 0;
}

const clap_plugin_gui_t &GuiTable() {
    static const clap_plugin_gui_t kGui = {
        // is_api_supported
        +[](const clap_plugin_t *, const char *api, bool isFloating) {
            return Supported(api, isFloating);
        },
        // get_preferred_api
        +[](const clap_plugin_t *, const char **api, bool *isFloating) {
            *api = WindowApi();
            // Embedded, so the picker lives in the host's own window where the tracks are.
            *isFloating = false;
            return true;
        },
        // create — the editor already exists per instance; a host that embeds attaches it
        // in set_parent below.
        +[](const clap_plugin_t *plugin, const char *, bool) {
            return Instance(plugin) != nullptr;
        },
        // destroy — only hides; the editor itself dies when the plugin is destroyed.
        +[](const clap_plugin_t *plugin) {
            InfoWindow *info = Instance(plugin);
            if (info != nullptr) {
                info->Hide();
            }
        },
        // set_scale — the YUP renderer is resolution independent; it takes the scale.
        +[](const clap_plugin_t *plugin, double scale) {
            InfoWindow *info = Instance(plugin);
            return info != nullptr && info->SetContentScale(static_cast<float>(scale));
        },
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
        // overridden: the editor draws itself at its fixed size regardless.
        +[](const clap_plugin_t *, uint32_t, uint32_t) { return true; },
        // set_parent
        +[](const clap_plugin_t *plugin, const clap_window_t *window) {
            InfoWindow *info = Instance(plugin);
            void *parent = IsHostWindow(window) ? ParentHandle(window) : nullptr;
            if (info == nullptr || parent == nullptr) {
                return false;
            }
            info->EmbedInto(parent);
            return true;
        },
        // set_transient
        +[](const clap_plugin_t *plugin, const clap_window_t *window) {
            InfoWindow *info = Instance(plugin);
            return info != nullptr && IsHostWindow(window) &&
                   (info->OwnTo(ParentHandle(window)), true);
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

// ------------------------------------------------------------------- the InfoWindow

/// Everything InfoWindow::impl_ points at. Created on the main thread by CreateInfoWindow;
/// the gui extension guarantees every later call lands there too.
struct YupWindow final : private yup::Timer {
    explicit YupWindow(Session &s) : state(std::make_unique<EditorState>(s)) {
        editor = std::make_unique<yup::Component>("bridgeEditor");

        editor->addAndMakeVisible(*state->trackBox);
        for (yup::Label *label : {state->connection.get(), state->project.get(),
                                  state->singer.get(), state->tempo.get(),
                                  state->transport.get()}) {
            editor->addAndMakeVisible(*label);
            label->setJustification(yup::Justification::centerLeft);
        }

        state->Layout();
        editor->setSize(kWindowWidth, kWindowHeight);
    }

    ~YupWindow() override { stopTimer(); }

    void Attach(void *parent) {
        // Mirrors YUP's own CLAP client: embed as a decoration-less child when a parent is
        // given, else as a free top-level window, then one initial refresh and the timer.
        auto flags = yup::ComponentNative::defaultFlags &
                     ~yup::ComponentNative::decoratedWindow;
        auto options = yup::ComponentNative::Options()
                           .withFlags(flags)
                           .withResizableWindow(false);
        editor->addToDesktop(options, parent);
        editor->setVisible(true);
        editor->attachedToNative();

        attached = true;
        state->Refresh(true);
        startTimerHz(4);
    }

    void timerCallback() override {
        if (attached) {
            state->Refresh();
        }
    }

    std::unique_ptr<yup::Component> editor;
    std::unique_ptr<EditorState> state;
    bool attached = false;
};

InfoWindow::~InfoWindow() {
    auto *state = static_cast<YupWindow *>(impl_);
    if (state == nullptr) {
        return;
    }
    delete state;
}

const clap_plugin_gui_t *InfoWindow::Extension() const { return &GuiTable(); }

void InfoWindow::Show() {
    auto *window = static_cast<YupWindow *>(impl_);
    if (!window->attached) {
        window->Attach(nullptr);
    }
    window->editor->setVisible(true);
}

void InfoWindow::Hide() {
    auto *window = static_cast<YupWindow *>(impl_);
    window->editor->setVisible(false);
}

void InfoWindow::EmbedInto(void *parent) {
    auto *window = static_cast<YupWindow *>(impl_);
    window->Attach(parent);
}

void InfoWindow::OwnTo(void *) {
    // YUP has no owner-window concept on its top-level windows; a floating window still
    // works, it merely outlives a closed host window. set_transient is optional in CLAP.
}

void InfoWindow::Retitle(const char *title) {
    // Only a floating window has a title bar; embedded children ignore it either way.
    (void)title;
}

bool InfoWindow::SetContentScale(float scale) {
    auto *window = static_cast<YupWindow *>(impl_);
    window->editor->contentScaleChanged(scale);
    return true;
}

void GuiRegister(const clap_plugin_t *plugin, InfoWindow *window) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Registry()[plugin] = window;
}

void GuiUnregister(const clap_plugin_t *plugin) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Registry().erase(plugin);
}

InfoWindow *CreateInfoWindow(Session *session, std::function<void()> onTrackPicked) {
    if (session == nullptr) {
        return nullptr;
    }
    auto *window = new InfoWindow();
    auto *state = new YupWindow(*session);
    state->state->onTrackPicked = std::move(onTrackPicked);
    window->impl_ = state;
    return window;
}

}  // namespace bridge

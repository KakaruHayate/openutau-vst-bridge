#pragma once

/*
 * The plugin's window: a track picker plus an info panel — project name, saved state,
 * connection state, tempo, transport, and per-track singer/engine. CLAP's gui extension
 * is the surface; the controls are the platform's own (Win32 common controls, AppKit),
 * because a window of five labels and one dropdown is not worth a UI framework's
 * dependency, build time or crash surface. A stub backend stands in on platforms without
 * a native backend (Linux), and then the plugin simply advertises no gui — a degraded
 * but working plugin whose track is still switchable from the host's own parameter UI.
 */

#include <clap/clap.h>

#include <functional>

namespace bridge {

class Session;

/// One instance's window, bound to that instance's session. All of its calls land on the
/// main thread, which is what CLAP requires of the gui extension. Created only through
/// CreateInfoWindow; destroyed only by delete, which destroys the window with it.
class InfoWindow final {
public:
    ~InfoWindow();
    InfoWindow(const InfoWindow &) = delete;
    InfoWindow &operator=(const InfoWindow &) = delete;

    /// The CLAP_EXT_GUI extension these calls arrive through. One table serves every
    /// instance; the callbacks reach their window through the registry below.
    const clap_plugin_gui_t *Extension() const;

    void Show();  ///< Lays out a fresh snapshot and makes the editor visible.
    void Hide();  ///< Hides the editor; the object outlives the call.
    void EmbedInto(void *parent);    ///< Attaches the editor to a host's native parent handle.
    void OwnTo(void *owner);         ///< Floating mode: takes the host window as owner.
    void Retitle(const char *title); ///< Floating mode: applies the host's suggested title.
    bool SetContentScale(float scale); ///< set_scale: forwards to the YUP renderer.

private:
    InfoWindow() = default;
    void *impl_ = nullptr;
    friend InfoWindow *CreateInfoWindow(Session *session, std::function<void()> onTrackPicked);
};

/// gui_win32.cpp, gui_cocoa.mm, or gui_stub.cpp. Null when the backend could not be
/// created — the plugin then advertises no gui, which is a degraded but working plugin.
/// `onTrackPicked` runs on the main thread after the user chooses a track; the plugin
/// uses it to nudge the host.
InfoWindow *CreateInfoWindow(Session *session, std::function<void()> onTrackPicked = {});

/// The gui extension's callbacks receive the clap_plugin_t, not the window, so each
/// instance registers itself here around plugin init/destroy. Main thread.
void GuiRegister(const clap_plugin_t *plugin, InfoWindow *window);
void GuiUnregister(const clap_plugin_t *plugin);

}  // namespace bridge

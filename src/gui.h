#pragma once

/*
 * The plugin's info window: project name, saved state, connection state, tempo, transport,
 * tracks. CLAP's gui extension is the surface; Win32 is the only implemented backend, so
 * elsewhere the plugin simply advertises no gui rather than a window that cannot exist.
 */

#include <clap/clap.h>

namespace bridge {

class Session;

#ifdef _WIN32

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

    void Show();  ///< Draws a fresh snapshot, starts the refresh timer and makes it visible.
    void Hide();  ///< Stops the timer and hides; the window outlives the call.
    void EmbedInto(void *parent);   ///< Re-parents the window into a host's hwnd (win32 api).
    void OwnTo(void *owner);        ///< Floating mode: takes the host window as owner.
    void Retitle(const char *title); ///< Floating mode: applies the host's suggested title.

private:
    InfoWindow() = default;
    void *impl_ = nullptr;
    friend InfoWindow *CreateInfoWindow(Session *session);
};

/// gui_win32.cpp. Null when the window class could not be registered — the plugin then
/// advertises no gui, which is a degraded but working plugin.
InfoWindow *CreateInfoWindow(Session *session);

/// The gui extension's callbacks receive the clap_plugin_t, not the window, so each
/// instance registers itself here around plugin init/destroy. Main thread.
void GuiRegister(const clap_plugin_t *plugin, InfoWindow *window);
void GuiUnregister(const clap_plugin_t *plugin);

#else

/// No window backend on this platform yet.
class InfoWindow final {
public:
    ~InfoWindow() = default;
    const clap_plugin_gui_t *Extension() const { return nullptr; }
};

inline InfoWindow *CreateInfoWindow(Session *) { return nullptr; }

inline void GuiRegister(const clap_plugin_t *, InfoWindow *) {}
inline void GuiUnregister(const clap_plugin_t *) {}

#endif

}  // namespace bridge

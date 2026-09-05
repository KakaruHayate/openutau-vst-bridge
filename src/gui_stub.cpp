/*
 * The no-gui backend, used when the build disables BRIDGE_ENABLE_GUI. The plugin then
 * advertises no gui extension at all: the window objects still exist so plugin.cpp has
 * one code path, but they hold nothing and answer nothing.
 */

#include "gui.h"

#include <functional>

namespace bridge {

InfoWindow::~InfoWindow() = default;

const clap_plugin_gui_t *InfoWindow::Extension() const { return nullptr; }

void InfoWindow::Show() {}
void InfoWindow::Hide() {}
void InfoWindow::EmbedInto(void *) {}
void InfoWindow::OwnTo(void *) {}
void InfoWindow::Retitle(const char *) {}
bool InfoWindow::SetContentScale(float) { return false; }

InfoWindow *CreateInfoWindow(Session *, std::function<void()>) { return nullptr; }

void GuiRegister(const clap_plugin_t *, InfoWindow *) {}
void GuiUnregister(const clap_plugin_t *) {}

}  // namespace bridge

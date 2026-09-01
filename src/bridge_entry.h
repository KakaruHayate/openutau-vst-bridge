#pragma once

// Implemented in plugin.cpp and given C linkage by bridge_entry.cpp. The split is what
// clap-wrapper's clap-first layout expects: the implementation is a static library, and
// each format's module links it against its own entry translation unit.
extern bool bridge_entry_init(const char *plugin_path);
extern void bridge_entry_deinit(void);
extern const void *bridge_entry_get_factory(const char *factory_id);

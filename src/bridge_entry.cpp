/*
 * The one exported symbol a CLAP module needs. Kept in its own translation unit so the
 * same static implementation can be linked into the CLAP, the VST3 and any later format
 * without each of them re-exporting clap_entry.
 */

#include <clap/clap.h>

#include "bridge_entry.h"

extern "C" {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

const CLAP_EXPORT struct clap_plugin_entry clap_entry = {
    CLAP_VERSION,
    bridge_entry_init,
    bridge_entry_deinit,
    bridge_entry_get_factory,
};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

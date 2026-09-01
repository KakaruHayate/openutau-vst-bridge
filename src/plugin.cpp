/*
 * The CLAP implementation. Right now it is a well-formed instrument that outputs
 * silence: enough for a host to scan, instantiate and run it, so the build and the
 * format wrapping can be verified before the bridge itself exists.
 *
 * See PROTOCOL.md for what this will eventually be talking to.
 */

#include <clap/clap.h>

#include <cstdio>
#include <cstring>

#include "bridge_entry.h"

namespace {

constexpr const char *kPluginId = "moe.kakaru.openutau-bridge";

const char *const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    kPluginId,
    "OpenUtau Bridge",
    "KakaruHayate",
    "https://github.com/KakaruHayate/openutau-vst-bridge",
    "",  // manual_url
    "",  // support_url
    "0.1.0",
    "Places audio rendered by OpenUtau onto the DAW timeline.",
    kFeatures,
};

/// One plugin instance.
struct Bridge {
    clap_plugin_t plugin{};
    const clap_host_t *host = nullptr;
    double sampleRate = 0.0;

    static Bridge *Of(const clap_plugin_t *plugin) {
        return static_cast<Bridge *>(plugin->plugin_data);
    }
};

// ------------------------------------------------------------------ audio ports

uint32_t AudioPortsCount(const clap_plugin_t *, bool isInput) {
    return isInput ? 0u : 1u;
}

bool AudioPortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                   clap_audio_port_info_t *info) {
    if (isInput || index != 0) {
        return false;
    }
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = {AudioPortsCount, AudioPortsGet};

// -------------------------------------------------------------------- lifecycle

bool PluginInit(const clap_plugin_t *) {
    return true;
}

void PluginDestroy(const clap_plugin_t *plugin) {
    delete Bridge::Of(plugin);
}

bool PluginActivate(const clap_plugin_t *plugin, double sampleRate, uint32_t, uint32_t) {
    // The wire format is fixed at 44.1 kHz (PROTOCOL.md 5.2), so whatever the host asks
    // for here is what the bridge will have to resample to.
    Bridge::Of(plugin)->sampleRate = sampleRate;
    return true;
}

void PluginDeactivate(const clap_plugin_t *) {}

bool PluginStartProcessing(const clap_plugin_t *) {
    return true;
}

void PluginStopProcessing(const clap_plugin_t *) {}

void PluginReset(const clap_plugin_t *) {}

clap_process_status PluginProcess(const clap_plugin_t *, const clap_process_t *process) {
    for (uint32_t port = 0; port < process->audio_outputs_count; ++port) {
        const clap_audio_buffer_t &out = process->audio_outputs[port];
        if (out.data32 == nullptr) {
            continue;
        }
        for (uint32_t channel = 0; channel < out.channel_count; ++channel) {
            if (out.data32[channel] != nullptr) {
                std::memset(out.data32[channel], 0, sizeof(float) * process->frames_count);
            }
        }
    }
    // CONTINUE rather than SLEEP: once a connection exists, audio can start at any
    // timeline position, and a sleeping plugin may not be called again promptly.
    return CLAP_PROCESS_CONTINUE;
}

const void *PluginGetExtension(const clap_plugin_t *, const char *id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &kAudioPorts;
    }
    return nullptr;
}

void PluginOnMainThread(const clap_plugin_t *) {}

// ----------------------------------------------------------------------- factory

uint32_t FactoryCount(const clap_plugin_factory_t *) {
    return 1;
}

const clap_plugin_descriptor_t *FactoryDescriptor(const clap_plugin_factory_t *,
                                                  uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t *FactoryCreate(const clap_plugin_factory_t *, const clap_host_t *host,
                                   const char *pluginId) {
    if (pluginId == nullptr || std::strcmp(pluginId, kPluginId) != 0) {
        return nullptr;
    }
    auto *bridge = new Bridge();
    bridge->host = host;
    bridge->plugin.desc = &kDescriptor;
    bridge->plugin.plugin_data = bridge;
    bridge->plugin.init = PluginInit;
    bridge->plugin.destroy = PluginDestroy;
    bridge->plugin.activate = PluginActivate;
    bridge->plugin.deactivate = PluginDeactivate;
    bridge->plugin.start_processing = PluginStartProcessing;
    bridge->plugin.stop_processing = PluginStopProcessing;
    bridge->plugin.reset = PluginReset;
    bridge->plugin.process = PluginProcess;
    bridge->plugin.get_extension = PluginGetExtension;
    bridge->plugin.on_main_thread = PluginOnMainThread;
    return &bridge->plugin;
}

const clap_plugin_factory_t kFactory = {FactoryCount, FactoryDescriptor, FactoryCreate};

}  // namespace

bool bridge_entry_init(const char *) {
    return true;
}

void bridge_entry_deinit(void) {}

const void *bridge_entry_get_factory(const char *factoryId) {
    if (factoryId != nullptr && std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0) {
        return &kFactory;
    }
    return nullptr;
}
